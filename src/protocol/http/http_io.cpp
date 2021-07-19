#include "protocol/http/http_io.h"

#include <assert.h>
#include <string.h>

#include "common/error.hpp"
#include "log/log.hpp"
#include "protocol/http/http_basic.h"
#include "protocol/http/http_message.h"

HttpResponseWriter::HttpResponseWriter(IoReaderWriter *io) {
    io_ = io;
    hdr = new HttpHeader();
    header_wrote = false;
    status = CONSTS_HTTP_OK;
    content_length = -1;
    written = 0;
    header_sent = false;
    nb_iovss_cache = 0;
    iovss_cache = nullptr;
    header_cache[0] = '\0';
}

HttpResponseWriter::~HttpResponseWriter() {
    coco_freep(hdr);
    coco_freepa(iovss_cache);
}

// IoReaderWriter *HttpResponseWriter::st_socket() { return io_; }

int HttpResponseWriter::final_request() {
    // write the header data in memory.
    if (!header_wrote) {
        WriteHeader(CONSTS_HTTP_OK);
    }

    // complete the chunked encoding.
    if (content_length == -1) {
        std::stringstream ss;
        ss << 0 << HTTP_CRLF << HTTP_CRLF;
        std::string ch = ss.str();
        return io_->Write((void *)ch.data(), (int)ch.length(), nullptr);
    }

    // flush when send with content length
    return Write(nullptr, 0);
}

HttpHeader *HttpResponseWriter::header() { return hdr; }

int HttpResponseWriter::Write(char *data, int size) {
    int ret = COCO_SUCCESS;

    // write the header data in memory.
    if (!header_wrote) {
        WriteHeader(CONSTS_HTTP_OK);
    }

    // whatever header is wrote, we should try to send header.
    if ((ret = SendHeader(data, size)) != COCO_SUCCESS) {
        coco_error("http: send header failed. ret=%d", ret);
        return ret;
    }

    // check the bytes send and content length.
    written += size;
    if (content_length != -1 && written > content_length) {
        ret = ERROR_HTTP_CONTENT_LENGTH;
        coco_error("http: exceed content length. ret=%d", ret);
        return ret;
    }

    // ignore nullptr content.
    if (!data) {
        return ret;
    }

    // directly send with content length
    if (content_length != -1) {
        return io_->Write((void *)data, size, nullptr);
    }

    // send in chunked encoding.
    int nb_size = snprintf(header_cache, HTTP_HEADER_CACHE_SIZE, "%x", size);

    iovec iovs[4];
    iovs[0].iov_base = (char *)header_cache;
    iovs[0].iov_len = (int)nb_size;
    iovs[1].iov_base = (char *)HTTP_CRLF;
    iovs[1].iov_len = 2;
    iovs[2].iov_base = (char *)data;
    iovs[2].iov_len = size;
    iovs[3].iov_base = (char *)HTTP_CRLF;
    iovs[3].iov_len = 2;

    ssize_t nwrite;
    if ((ret = io_->Writev(iovs, 4, &nwrite)) != COCO_SUCCESS) {
        return ret;
    }

    return ret;
}

int HttpResponseWriter::Writev(iovec *iov, int iovcnt, ssize_t *pnwrite) {
    int ret = COCO_SUCCESS;

    // when header not ready, or not chunked, send one by one.
    if (!header_wrote || content_length != -1) {
        ssize_t nwrite = 0;
        for (int i = 0; i < iovcnt; i++) {
            iovec *piovc = iov + i;
            nwrite += piovc->iov_len;
            if ((ret = Write((char *)piovc->iov_base, (int)piovc->iov_len)) != COCO_SUCCESS) {
                return ret;
            }
        }

        if (pnwrite) {
            *pnwrite = nwrite;
        }

        return ret;
    }

    // ignore nullptr content.
    if (iovcnt <= 0) {
        return ret;
    }

    // send in chunked encoding.
    int nb_iovss = 3 + iovcnt;
    iovec *iovss = iovss_cache;
    if (nb_iovss_cache < nb_iovss) {
        coco_freepa(iovss_cache);
        nb_iovss_cache = nb_iovss;
        iovss = iovss_cache = new iovec[nb_iovss];
    }

    // send in chunked encoding.

    // chunk size.
    int size = 0;
    for (int i = 0; i < iovcnt; i++) {
        iovec *data_iov = iov + i;
        size += (int)data_iov->iov_len;
    }
    written += size;

    // chunk header
    int nb_size = snprintf(header_cache, HTTP_HEADER_CACHE_SIZE, "%x", size);
    iovec *iovs = iovss;
    iovs[0].iov_base = (char *)header_cache;
    iovs[0].iov_len = (int)nb_size;
    iovs++;

    // chunk header eof.
    iovs[0].iov_base = (char *)HTTP_CRLF;
    iovs[0].iov_len = 2;
    iovs++;

    // chunk body.
    for (int i = 0; i < iovcnt; i++) {
        iovec *data_iov = iov + i;
        iovs[0].iov_base = (char *)data_iov->iov_base;
        iovs[0].iov_len = (int)data_iov->iov_len;
        iovs++;
    }

    // chunk body eof.
    iovs[0].iov_base = (char *)HTTP_CRLF;
    iovs[0].iov_len = 2;
    iovs++;

    // sendout all ioves.
    ssize_t nwrite;
    if ((ret = write_large_iovs(io_, iovss, nb_iovss, &nwrite)) != COCO_SUCCESS) {
        return ret;
    }

    if (pnwrite) {
        *pnwrite = nwrite;
    }

    return ret;
}

void HttpResponseWriter::WriteHeader(int code) {
    if (header_wrote) {
        coco_warn("http: multiple write_header calls, code=%d", code);
        return;
    }

    header_wrote = true;
    status = code;

    // parse the content length from header.
    content_length = hdr->content_length();
}

int HttpResponseWriter::SendHeader(char *data, int size) {
    int ret = COCO_SUCCESS;

    if (header_sent) {
        return ret;
    }
    header_sent = true;

    std::stringstream ss;

    // status_line
    ss << "HTTP/1.1 " << status << " " << generate_http_status_text(status) << HTTP_CRLF;

    // detect content type
    if (go_http_body_allowd(status)) {
        if (hdr->content_type().empty()) {
            hdr->set_content_type(go_http_detect(data, size));
        }
    }

    // chunked encoding
    if (content_length == -1) {
        hdr->set("Transfer-Encoding", "chunked");
    }

    // keep alive to make vlc happy.
    hdr->set("Connection", "Keep-Alive");

    // write headers
    hdr->write(ss);

    // header_eof
    ss << HTTP_CRLF;

    std::string buf = ss.str();
    hdr->set_header_length(buf.length());
    return io_->Write((void *)buf.c_str(), buf.length(), nullptr);
}

HttpResponseReader::HttpResponseReader(HttpMessage *msg, IoReaderWriter *io) {
    io_ = io;
    owner = msg;
    is_eof = false;
    nb_total_read = 0;
    nb_left_chunk = 0;
    buffer = nullptr;
    nb_chunk = 0;
}

HttpResponseReader::~HttpResponseReader() {}

int HttpResponseReader::initialize(FastBuffer *body) {
    int ret = COCO_SUCCESS;

    nb_chunk = 0;
    nb_left_chunk = 0;
    nb_total_read = 0;
    buffer = body;

    return ret;
}

bool HttpResponseReader::eof() { return is_eof; }

int HttpResponseReader::ReadFull(char *data, int nb_data, int *nb_read) {
    int ret = COCO_SUCCESS;
    int nsize = 0;
    int nleft = nb_data;
    while (true) {
        ret = Read(data, nleft, &nsize);
        if (ret != COCO_SUCCESS) {
            return ret;
        }

        data = data + nsize;
        nleft = nleft - nsize;
        nsize = 0;
        if (nleft < 0) {
            ret = ERROR_SYSTEM_IO_INVALID;
            return ret;
        }

        if (nleft == 0) {
            *nb_read = nb_data;
            break;
        }
    }

    return ret;
}

int HttpResponseReader::Read(char *data, int nb_data, int *nb_read) {
    int ret = COCO_SUCCESS;

    if (is_eof) {
        ret = ERROR_HTTP_RESPONSE_EOF;
        coco_error("http: response EOF. ret=%d, cont len=%d", ret, (int)owner->content_length());
        return ret;
    }

    // chunked encoding.
    if (owner->is_chunked()) {
        return ReadChunked(data, nb_data, nb_read);
    }

    // read by specified content-length
    if (owner->content_length() != -1) {
        int max = (int)owner->content_length() - (int)nb_total_read;
        if (max <= 0) {
            is_eof = true;
            return ret;
        }

        // change the max to read.
        nb_data = coco_min(nb_data, max);
        return ReadSpecified(data, nb_data, nb_read);
    }

    // infinite chunked mode, directly read.
    if (owner->is_infinite_chunked()) {
        assert(!owner->is_chunked() && owner->content_length() == -1);
        return ReadSpecified(data, nb_data, nb_read);
    }

    // infinite chunked mode, but user not set it,
    // we think there is no data left.
    is_eof = true;

    return ret;
}

int HttpResponseReader::ReadChunked(char *data, int nb_data, int *nb_read) {
    int ret = COCO_SUCCESS;

    // when no bytes left in chunk,
    // parse the chunk length first.
    if (nb_left_chunk <= 0) {
        char *at = nullptr;
        int length = 0;
        while (!at) {
            // find the CRLF of chunk header end.
            char *start = buffer->bytes();
            char *end = start + buffer->size();
            for (char *p = start; p < end - 1; p++) {
                if (p[0] == HTTP_CR && p[1] == HTTP_LF) {
                    // invalid chunk, ignore.
                    if (p == start) {
                        ret = ERROR_HTTP_INVALID_CHUNK_HEADER;
                        coco_error("chunk header start with CRLF. ret=%d", ret);
                        return ret;
                    }
                    length = (int)(p - start + 2);
                    at = buffer->read_slice(length);
                    break;
                }
            }

            // got at, ok.
            if (at) {
                break;
            }

            // when empty, only grow 1bytes, but the buffer will cache more.
            if ((ret = buffer->grow(io_, buffer->size() + 1)) != COCO_SUCCESS) {
                if (!coco_is_client_gracefully_close(ret)) {
                    coco_error("read body from server failed. ret=%d", ret);
                }
                return ret;
            }
        }
        assert(length >= 3);

        // it's ok to set the pos and pos+1 to nullptr.
        at[length - 1] = 0;
        at[length - 2] = 0;

        // size is the bytes size, excludes the chunk header and end CRLF.
        int ilength = (int)::strtol(at, nullptr, 16);
        if (ilength < 0) {
            ret = ERROR_HTTP_INVALID_CHUNK_HEADER;
            coco_error("chunk header negative, length=%d. ret=%d", ilength, ret);
            return ret;
        }

        // all bytes in chunk is left now.
        nb_chunk = nb_left_chunk = ilength;
    }

    if (nb_chunk <= 0) {
        // for the last chunk, eof.
        is_eof = true;
    } else {
        // for not the last chunk, there must always exists bytes.
        // left bytes in chunk, read some.
        assert(nb_left_chunk);

        int nb_bytes = coco_min(nb_left_chunk, nb_data);
        ret = ReadSpecified(data, nb_bytes, &nb_bytes);

        // the nb_bytes used for output already read size of bytes.
        if (nb_read) {
            *nb_read = nb_bytes;
        }
        nb_left_chunk -= nb_bytes;
        coco_info("http: read %d bytes of chunk", nb_bytes);

        // error or still left bytes in chunk, ignore and read in future.
        if (nb_left_chunk > 0 || (ret != COCO_SUCCESS)) {
            return ret;
        }
        coco_info("http: read total chunk %dB", nb_chunk);
    }

    // for both the last or not, the CRLF of chunk payload end.
    if ((ret = buffer->grow(io_, 2)) != COCO_SUCCESS) {
        if (!coco_is_client_gracefully_close(ret)) {
            coco_error("read EOF of chunk from server failed. ret=%d", ret);
        }
        return ret;
    }
    buffer->read_slice(2);

    return ret;
}

int HttpResponseReader::ReadSpecified(char *data, int nb_data, int *nb_read) {
    int ret = COCO_SUCCESS;

    if (buffer->size() <= 0) {
        // when empty, only grow 1bytes, but the buffer will cache more.
        if ((ret = buffer->grow(io_, 1)) != COCO_SUCCESS) {
            if (!coco_is_client_gracefully_close(ret)) {
                coco_error("read body from server failed. ret=%d", ret);
            }
            return ret;
        }
    }

    int nb_bytes = coco_min(nb_data, buffer->size());

    // read data to buffer.
    assert(nb_bytes);
    char *p = buffer->read_slice(nb_bytes);
    memcpy(data, p, nb_bytes);
    if (nb_read) {
        *nb_read = nb_bytes;
    }

    // increase the total read to determine whether EOF.
    nb_total_read += nb_bytes;

    // for not chunked and specified content length.
    if (!owner->is_chunked() && owner->content_length() != -1) {
        // when read completed, eof.
        if (nb_total_read >= (int)owner->content_length()) {
            is_eof = true;
        }
    }

    return ret;
}

// get the status text of code.
std::string generate_http_status_text(int status) {
    static std::map<int, std::string> _status_map;
    if (_status_map.empty()) {
        _status_map[CONSTS_HTTP_Continue] = CONSTS_HTTP_Continue_str;
        _status_map[CONSTS_HTTP_SwitchingProtocols] = CONSTS_HTTP_SwitchingProtocols_str;
        _status_map[CONSTS_HTTP_OK] = CONSTS_HTTP_OK_str;
        _status_map[CONSTS_HTTP_Created] = CONSTS_HTTP_Created_str;
        _status_map[CONSTS_HTTP_Accepted] = CONSTS_HTTP_Accepted_str;
        _status_map[CONSTS_HTTP_NonAuthoritativeInformation] =
            CONSTS_HTTP_NonAuthoritativeInformation_str;
        _status_map[CONSTS_HTTP_NoContent] = CONSTS_HTTP_NoContent_str;
        _status_map[CONSTS_HTTP_ResetContent] = CONSTS_HTTP_ResetContent_str;
        _status_map[CONSTS_HTTP_PartialContent] = CONSTS_HTTP_PartialContent_str;
        _status_map[CONSTS_HTTP_MultipleChoices] = CONSTS_HTTP_MultipleChoices_str;
        _status_map[CONSTS_HTTP_MovedPermanently] = CONSTS_HTTP_MovedPermanently_str;
        _status_map[CONSTS_HTTP_Found] = CONSTS_HTTP_Found_str;
        _status_map[CONSTS_HTTP_SeeOther] = CONSTS_HTTP_SeeOther_str;
        _status_map[CONSTS_HTTP_NotModified] = CONSTS_HTTP_NotModified_str;
        _status_map[CONSTS_HTTP_UseProxy] = CONSTS_HTTP_UseProxy_str;
        _status_map[CONSTS_HTTP_TemporaryRedirect] = CONSTS_HTTP_TemporaryRedirect_str;
        _status_map[CONSTS_HTTP_BadRequest] = CONSTS_HTTP_BadRequest_str;
        _status_map[CONSTS_HTTP_Unauthorized] = CONSTS_HTTP_Unauthorized_str;
        _status_map[CONSTS_HTTP_PaymentRequired] = CONSTS_HTTP_PaymentRequired_str;
        _status_map[CONSTS_HTTP_Forbidden] = CONSTS_HTTP_Forbidden_str;
        _status_map[CONSTS_HTTP_NotFound] = CONSTS_HTTP_NotFound_str;
        _status_map[CONSTS_HTTP_MethodNotAllowed] = CONSTS_HTTP_MethodNotAllowed_str;
        _status_map[CONSTS_HTTP_NotAcceptable] = CONSTS_HTTP_NotAcceptable_str;
        _status_map[CONSTS_HTTP_ProxyAuthenticationRequired] =
            CONSTS_HTTP_ProxyAuthenticationRequired_str;
        _status_map[CONSTS_HTTP_RequestTimeout] = CONSTS_HTTP_RequestTimeout_str;
        _status_map[CONSTS_HTTP_Conflict] = CONSTS_HTTP_Conflict_str;
        _status_map[CONSTS_HTTP_Gone] = CONSTS_HTTP_Gone_str;
        _status_map[CONSTS_HTTP_LengthRequired] = CONSTS_HTTP_LengthRequired_str;
        _status_map[CONSTS_HTTP_PreconditionFailed] = CONSTS_HTTP_PreconditionFailed_str;
        _status_map[CONSTS_HTTP_RequestEntityTooLarge] = CONSTS_HTTP_RequestEntityTooLarge_str;
        _status_map[CONSTS_HTTP_RequestURITooLarge] = CONSTS_HTTP_RequestURITooLarge_str;
        _status_map[CONSTS_HTTP_UnsupportedMediaType] = CONSTS_HTTP_UnsupportedMediaType_str;
        _status_map[CONSTS_HTTP_RequestedRangeNotSatisfiable] =
            CONSTS_HTTP_RequestedRangeNotSatisfiable_str;
        _status_map[CONSTS_HTTP_ExpectationFailed] = CONSTS_HTTP_ExpectationFailed_str;
        _status_map[CONSTS_HTTP_InternalServerError] = CONSTS_HTTP_InternalServerError_str;
        _status_map[CONSTS_HTTP_NotImplemented] = CONSTS_HTTP_NotImplemented_str;
        _status_map[CONSTS_HTTP_BadGateway] = CONSTS_HTTP_BadGateway_str;
        _status_map[CONSTS_HTTP_ServiceUnavailable] = CONSTS_HTTP_ServiceUnavailable_str;
        _status_map[CONSTS_HTTP_GatewayTimeout] = CONSTS_HTTP_GatewayTimeout_str;
        _status_map[CONSTS_HTTP_HTTPVersionNotSupported] = CONSTS_HTTP_HTTPVersionNotSupported_str;
    }

    std::string status_text;
    if (_status_map.find(status) == _status_map.end()) {
        status_text = "Status Unknown";
    } else {
        status_text = _status_map[status];
    }

    return status_text;
}

// bodyAllowedForStatus reports whether a given response status code
// permits a body.  See RFC2616, section 4.4.
bool go_http_body_allowd(int status) {
    if (status >= 100 && status <= 199) {
        return false;
    } else if (status == 204 || status == 304) {
        return false;
    }

    return true;
}

// DetectContentType implements the algorithm described
// at http://mimesniff.spec.whatwg.org/ to determine the
// Content-Type of the given data.  It considers at most the
// first 512 bytes of data.  DetectContentType always returns
// a valid MIME type: if it cannot determine a more specific one, it
// returns "application/octet-stream".
std::string go_http_detect(char *data, int size) {
    // detect only when data specified.
    if (data) {
    }
    return "application/octet-stream";  // fallback
}

int go_http_error(HttpResponseWriter *w, int code) {
    return go_http_error(w, code, generate_http_status_text(code));
}

int go_http_error(HttpResponseWriter *w, int code, std::string error) {
    int ret = COCO_SUCCESS;

    w->header()->set_content_type("text/plain; charset=utf-8");
    w->header()->set_content_length(error.length());
    w->WriteHeader(code);
    w->Write((char *)error.data(), (int)error.length());

    return ret;
}