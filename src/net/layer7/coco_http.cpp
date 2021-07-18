#include "net/layer7/coco_http.hpp"

#include <algorithm>
#include <assert.h>
#include <netdb.h>
#include <string.h>

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "net/coco_socket.hpp"

// get the status text of code.
std::string generate_http_status_text(int status) {
  static std::map<int, std::string> _status_map;
  if (_status_map.empty()) {
    _status_map[CONSTS_HTTP_Continue] = CONSTS_HTTP_Continue_str;
    _status_map[CONSTS_HTTP_SwitchingProtocols] =
        CONSTS_HTTP_SwitchingProtocols_str;
    _status_map[CONSTS_HTTP_OK] = CONSTS_HTTP_OK_str;
    _status_map[CONSTS_HTTP_Created] = CONSTS_HTTP_Created_str;
    _status_map[CONSTS_HTTP_Accepted] = CONSTS_HTTP_Accepted_str;
    _status_map[CONSTS_HTTP_NonAuthoritativeInformation] =
        CONSTS_HTTP_NonAuthoritativeInformation_str;
    _status_map[CONSTS_HTTP_NoContent] = CONSTS_HTTP_NoContent_str;
    _status_map[CONSTS_HTTP_ResetContent] = CONSTS_HTTP_ResetContent_str;
    _status_map[CONSTS_HTTP_PartialContent] = CONSTS_HTTP_PartialContent_str;
    _status_map[CONSTS_HTTP_MultipleChoices] = CONSTS_HTTP_MultipleChoices_str;
    _status_map[CONSTS_HTTP_MovedPermanently] =
        CONSTS_HTTP_MovedPermanently_str;
    _status_map[CONSTS_HTTP_Found] = CONSTS_HTTP_Found_str;
    _status_map[CONSTS_HTTP_SeeOther] = CONSTS_HTTP_SeeOther_str;
    _status_map[CONSTS_HTTP_NotModified] = CONSTS_HTTP_NotModified_str;
    _status_map[CONSTS_HTTP_UseProxy] = CONSTS_HTTP_UseProxy_str;
    _status_map[CONSTS_HTTP_TemporaryRedirect] =
        CONSTS_HTTP_TemporaryRedirect_str;
    _status_map[CONSTS_HTTP_BadRequest] = CONSTS_HTTP_BadRequest_str;
    _status_map[CONSTS_HTTP_Unauthorized] = CONSTS_HTTP_Unauthorized_str;
    _status_map[CONSTS_HTTP_PaymentRequired] = CONSTS_HTTP_PaymentRequired_str;
    _status_map[CONSTS_HTTP_Forbidden] = CONSTS_HTTP_Forbidden_str;
    _status_map[CONSTS_HTTP_NotFound] = CONSTS_HTTP_NotFound_str;
    _status_map[CONSTS_HTTP_MethodNotAllowed] =
        CONSTS_HTTP_MethodNotAllowed_str;
    _status_map[CONSTS_HTTP_NotAcceptable] = CONSTS_HTTP_NotAcceptable_str;
    _status_map[CONSTS_HTTP_ProxyAuthenticationRequired] =
        CONSTS_HTTP_ProxyAuthenticationRequired_str;
    _status_map[CONSTS_HTTP_RequestTimeout] = CONSTS_HTTP_RequestTimeout_str;
    _status_map[CONSTS_HTTP_Conflict] = CONSTS_HTTP_Conflict_str;
    _status_map[CONSTS_HTTP_Gone] = CONSTS_HTTP_Gone_str;
    _status_map[CONSTS_HTTP_LengthRequired] = CONSTS_HTTP_LengthRequired_str;
    _status_map[CONSTS_HTTP_PreconditionFailed] =
        CONSTS_HTTP_PreconditionFailed_str;
    _status_map[CONSTS_HTTP_RequestEntityTooLarge] =
        CONSTS_HTTP_RequestEntityTooLarge_str;
    _status_map[CONSTS_HTTP_RequestURITooLarge] =
        CONSTS_HTTP_RequestURITooLarge_str;
    _status_map[CONSTS_HTTP_UnsupportedMediaType] =
        CONSTS_HTTP_UnsupportedMediaType_str;
    _status_map[CONSTS_HTTP_RequestedRangeNotSatisfiable] =
        CONSTS_HTTP_RequestedRangeNotSatisfiable_str;
    _status_map[CONSTS_HTTP_ExpectationFailed] =
        CONSTS_HTTP_ExpectationFailed_str;
    _status_map[CONSTS_HTTP_InternalServerError] =
        CONSTS_HTTP_InternalServerError_str;
    _status_map[CONSTS_HTTP_NotImplemented] = CONSTS_HTTP_NotImplemented_str;
    _status_map[CONSTS_HTTP_BadGateway] = CONSTS_HTTP_BadGateway_str;
    _status_map[CONSTS_HTTP_ServiceUnavailable] =
        CONSTS_HTTP_ServiceUnavailable_str;
    _status_map[CONSTS_HTTP_GatewayTimeout] = CONSTS_HTTP_GatewayTimeout_str;
    _status_map[CONSTS_HTTP_HTTPVersionNotSupported] =
        CONSTS_HTTP_HTTPVersionNotSupported_str;
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
  return "application/octet-stream"; // fallback
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

HttpHeader::HttpHeader() { header_length = 0; }

HttpHeader::~HttpHeader() {}

void HttpHeader::set(std::string key, std::string value) {
  headers[key] = value;
}

std::string HttpHeader::get(std::string key) {
  std::string v;

  if (headers.find(key) != headers.end()) {
    v = headers[key];
  }

  return v;
}

int64_t HttpHeader::content_length() {
  std::string cl = get("Content-Length");

  if (cl.empty()) {
    return -1;
  }

  return (int64_t)::atof(cl.c_str());
}

void HttpHeader::set_content_length(int64_t size) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%ld", size);
  set("Content-Length", buf);
}

int64_t HttpHeader::get_header_length() { return header_length; }

void HttpHeader::set_header_length(int64_t size) { header_length = size; }

std::string HttpHeader::content_type() { return get("Content-Type"); }

void HttpHeader::set_content_type(std::string ct) { set("Content-Type", ct); }

void HttpHeader::write(std::stringstream &ss) {
  std::map<std::string, std::string>::iterator it;
  for (it = headers.begin(); it != headers.end(); ++it) {
    ss << it->first << ": " << it->second << HTTP_CRLF;
  }
}

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
      if ((ret = Write((char *)piovc->iov_base, (int)piovc->iov_len)) !=
          COCO_SUCCESS) {
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
  ss << "HTTP/1.1 " << status << " " << generate_http_status_text(status)
     << HTTP_CRLF;

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
    coco_error("http: response EOF. ret=%d, cont len=%d", ret,
               (int)owner->content_length());
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

HttpUri::HttpUri() { port = DEFAULT_HTTP_PORT; }

HttpUri::~HttpUri() {}

int HttpUri::initialize(std::string _url) {
  int ret = COCO_SUCCESS;

  url = _url;
  const char *purl = url.c_str();

  http_parser_url hp_u;
  if ((ret = http_parser_parse_url(purl, url.length(), 0, &hp_u)) != 0) {
    int code = ret;
    ret = ERROR_HTTP_PARSE_URI;

    coco_error("parse url %s failed, code=%d, ret=%d", purl, code, ret);
    return ret;
  }

  std::string field = get_uri_field(url, &hp_u, UF_SCHEMA);
  if (!field.empty()) {
    schema = field;
  }

  host = get_uri_field(url, &hp_u, UF_HOST);

  field = get_uri_field(url, &hp_u, UF_PORT);
  if (!field.empty()) {
    port = atoi(field.c_str());
  }

  path = get_uri_field(url, &hp_u, UF_PATH);
  coco_info("parse path %s success", path.c_str());

  query = get_uri_field(url, &hp_u, UF_QUERY);
  coco_info("parse query %s success", query.c_str());

  raw_path = path;
  if (!query.empty()) {
    raw_path += "?" + query;
  }

  return ret;
}

std::string HttpUri::get_uri_field(std::string uri, http_parser_url *hp_u,
                                   http_parser_url_fields field) {
  if ((hp_u->field_set & (1 << field)) == 0) {
    return "";
  }

  coco_dbg("uri field matched, off=%d, len=%d, value=%.*s",
           hp_u->field_data[field].off, hp_u->field_data[field].len,
           hp_u->field_data[field].len,
           uri.c_str() + hp_u->field_data[field].off);

  int offset = hp_u->field_data[field].off;
  int len = hp_u->field_data[field].len;

  return uri.substr(offset, len);
}

HttpParser::HttpParser() {
  buffer = new FastBuffer();
  expect_field_name = false;
  header_parsed = 0;
}

HttpParser::~HttpParser() { coco_freep(buffer); }

int HttpParser::initialize(enum http_parser_type type) {
  int ret = COCO_SUCCESS;

  memset(&settings, 0, sizeof(settings));
  settings.on_message_begin = on_message_begin;
  settings.on_url = on_url;
  settings.on_header_field = on_header_field;
  settings.on_header_value = on_header_value;
  settings.on_headers_complete = on_headers_complete;
  settings.on_body = on_body;
  settings.on_message_complete = on_message_complete;

  http_parser_init(&parser, type);
  // callback object ptr.
  parser.data = (void *)this;
  p_body_start = nullptr;
  p_header_tail = nullptr;

  return ret;
}

int HttpParser::parse_message(IoReaderWriter *io_, HttpServerConn *conn,
                              HttpMessage **ppmsg) {
  *ppmsg = nullptr;

  int ret = COCO_SUCCESS;

  // reset request data.
  field_name = "";
  field_value = "";
  expect_field_name = true;
  state = HttpParseStateInit;
  header = http_parser();
  url = "";
  headers.clear();
  header_parsed = 0;

  // do parse
  if ((ret = parse_message_imp(io_)) != COCO_SUCCESS) {
    if (!coco_is_client_gracefully_close(ret)) {
      coco_error("parse http msg failed. ret=%d", ret);
    }
    return ret;
  }

  // create msg
  HttpMessage *msg = new HttpMessage(io_, conn);

  // initalize http msg, parse url.
  if ((ret = msg->update(url, &header, buffer, headers)) != COCO_SUCCESS) {
    coco_error("initialize http msg failed. ret=%d", ret);
    coco_freep(msg);
    return ret;
  }

  // parse ok, return the msg.
  *ppmsg = msg;

  return ret;
}

int HttpParser::parse_message_imp(IoReaderWriter *io_) {
  int ret = COCO_SUCCESS;

  // while (true) {
  //     ssize_t nparsed = 0;

  //     // when got entire http header, parse it.
  //     char* start = buffer->bytes();
  //     char* end = start + buffer->size();
  //     for (char* p = start; p <= end - 4; p++) {
  //         // SRS_HTTP_CRLFCRLF "\r\n\r\n" // 0x0D0A0D0A
  //         if (p[0] == CONSTS_CR && p[1] == CONSTS_LF && p[2] == CONSTS_CR &&
  //         p[3] == CONSTS_LF) {
  //             nparsed = http_parser_execute(&parser, &settings,
  //             buffer->bytes(), buffer->size()); coco_info("buffer=%d,
  //             nparsed=%d, header=%d", buffer->size(), (int)nparsed,
  //             header_parsed);
  //             // break;
  //         }
  //     }

  //     // consume the parsed bytes.
  //     if (nparsed && header_parsed) {
  //         buffer->read_slice(header_parsed);
  //         coco_info("temp: size=%d,  %.*s", buffer->size(), buffer->size(),
  //         buffer->bytes());
  //     }

  //     // ok atleast header completed,
  //     // never wait for body completed, for maybe chunked.
  //     if (state == HttpParseStateHeaderComplete || state ==
  //     HttpParseStateMessageComplete) {
  //         break;
  //     }

  //     // when nothing parsed, read more to parse.
  //     if (nparsed == 0) {
  //         // when requires more, only grow 1bytes, but the buffer will cache
  //         more. if ((ret = buffer->grow(io_, buffer->size() + 1)) !=
  //         COCO_SUCCESS) {
  //             if (!coco_is_client_gracefully_close(ret)) {
  //                 coco_error("read body from server failed. ret=%d", ret);
  //             }
  //             return ret;
  //         }
  //     }
  // }

  while (true) {
    int32_t consumed = 0;
    if (buffer->size() > 0) {
      ssize_t nparsed = http_parser_execute(&parser, &settings, buffer->bytes(),
                                            buffer->size());

      // When buffer consumed these bytes, it's dropped so the new ptr is
      // actually the HTTP body. But http-parser doesn't indicate the specific
      // sizeof header, so we must finger it out.
      // @remark We shouldn't use on_body, because it only works for normal
      // case, and losts the chunk header and length.
      if (p_header_tail && buffer->bytes() < p_body_start) {
        for (const char *p = p_header_tail; p <= p_body_start - 4; p++) {
          if (p[0] == CONSTS_CR && p[1] == CONSTS_LF && p[2] == CONSTS_CR &&
              p[3] == CONSTS_LF) {
            consumed = int32_t(p + 4 - buffer->bytes());
            break;
          }
        }
      }
      coco_info("size=%d, nparsed=%d, consumed=%d", buffer->size(),
                (int)nparsed, consumed);

      // Only consume the header bytes.
      buffer->read_slice(consumed);

      // Done when header completed, never wait for body completed, because it
      // maybe chunked.
      if (state >= HttpParseStateHeaderComplete) {
        break;
      }
    }

    // when requires more, only grow 1bytes, but the buffer will cache more.
    if ((ret = buffer->grow(io_, buffer->size() + 1)) != COCO_SUCCESS) {
      if (!coco_is_client_gracefully_close(ret)) {
        coco_error("read body from server failed. ret=%d", ret);
      }
      return ret;
    }
  }

  // parse last header.
  if (!field_name.empty() && !field_value.empty()) {
    headers.push_back(std::make_pair(field_name, field_value));
  }

  return ret;
}

int HttpParser::on_message_begin(http_parser *parser) {
  HttpParser *obj = (HttpParser *)parser->data;
  assert(obj);

  obj->state = HttpParseStateStart;

  coco_info("***MESSAGE BEGIN***");

  return 0;
}

int HttpParser::on_headers_complete(http_parser *parser) {
  HttpParser *obj = (HttpParser *)parser->data;
  assert(obj);

  obj->header = *parser;
  // save the parser when header parse completed.
  obj->state = HttpParseStateHeaderComplete;
  obj->header_parsed = (int)parser->nread;

  coco_info("***HEADERS COMPLETE***");

  // see http_parser.c:1570, return 1 to skip body.
  return 0;
}

int HttpParser::on_message_complete(http_parser *parser) {
  HttpParser *obj = (HttpParser *)parser->data;
  assert(obj);

  // save the parser when body parse completed.
  obj->state = HttpParseStateMessageComplete;
  obj->p_body_start = obj->buffer->bytes() + obj->buffer->size();

  coco_info("***MESSAGE COMPLETE***\n");

  return 0;
}

int HttpParser::on_url(http_parser *parser, const char *at, size_t length) {
  HttpParser *obj = (HttpParser *)parser->data;
  assert(obj);

  if (length > 0) {
    obj->url.append(at, (int)length);
  }
  obj->p_header_tail = at;

  coco_info("Method: %d, Url: %.*s", parser->method, (int)length, at);

  return 0;
}

int HttpParser::on_header_field(http_parser *parser, const char *at,
                                size_t length) {
  HttpParser *obj = (HttpParser *)parser->data;
  assert(obj);

  // field value=>name, reap the field.
  if (!obj->expect_field_name) {
    obj->headers.push_back(std::make_pair(obj->field_name, obj->field_value));

    // reset the field name when parsed.
    obj->field_name = "";
    obj->field_value = "";
  }
  obj->expect_field_name = true;

  if (length > 0) {
    obj->field_name.append(at, (int)length);
  }
  obj->p_header_tail = at;
  coco_info("Header field(%d bytes): %.*s", (int)length, (int)length, at);
  return 0;
}

int HttpParser::on_header_value(http_parser *parser, const char *at,
                                size_t length) {
  HttpParser *obj = (HttpParser *)parser->data;
  assert(obj);

  if (length > 0) {
    obj->field_value.append(at, (int)length);
  }
  obj->expect_field_name = false;
  obj->p_header_tail = at;
  coco_info("Header value(%d bytes): %.*s", (int)length, (int)length, at);
  return 0;
}

int HttpParser::on_body(http_parser *parser, const char *at, size_t length) {
  // HttpParser* obj = (HttpParser*)parser->data;
  // assert(obj);

  coco_info("Body:len:%d,  %.*s", (int)length, (int)length, at);

  return 0;
}

HttpMessage::HttpMessage(IoReaderWriter *io, HttpServerConn *c) {
  conn = c;
  chunked = false;
  infinite_chunked = false;
  keep_alive = true;
  _uri = new HttpUri();
  _body = new HttpResponseReader(this, io);
  jsonp = false;
}

HttpMessage::~HttpMessage() {
  coco_freep(_body);
  coco_freep(_uri);
}

int HttpMessage::update_buffer(FastBuffer *body) {
  int ret = COCO_SUCCESS;
  if ((ret = _body->initialize(body)) != COCO_SUCCESS) {
    return ret;
  }
  return ret;
}

int HttpMessage::update(std::string url_, http_parser *header, FastBuffer *body,
                        std::vector<HttpHeaderField> &headers) {
  int ret = COCO_SUCCESS;

  _url = url_;
  _header = *header;
  _headers = headers;

  // whether chunked.
  std::string transfer_encoding = get_request_header("Transfer-Encoding");
  chunked = (transfer_encoding == "chunked");

  // whether keep alive.
  keep_alive = http_should_keep_alive(header);

  // set the buffer.
  if ((ret = _body->initialize(body)) != COCO_SUCCESS) {
    return ret;
  }

  // parse uri from url.
  std::string _host = get_request_header("Host");

  // use server public ip when no host specified.
  // to make telnet happy.
  if (_host.empty()) {
    _host = get_public_internet_address();
  }

  // parse uri to schema/server:port/path?query
  std::string uri_ = "http://" + _host + _url;
  if ((ret = _uri->initialize(uri_)) != COCO_SUCCESS) {
    return ret;
  }

  // must format as key=value&...&keyN=valueN
  std::string q = _uri->get_query();
  size_t pos = std::string::npos;
  while (!q.empty()) {
    std::string k = q;
    if ((pos = q.find("=")) != std::string::npos) {
      k = q.substr(0, pos);
      q = q.substr(pos + 1);
    } else {
      q = "";
    }

    std::string v = q;
    if ((pos = q.find("&")) != std::string::npos) {
      v = q.substr(0, pos);
      q = q.substr(pos + 1);
    } else {
      q = "";
    }

    _query[k] = v;
  }

  // parse ext.
  _ext = _uri->get_path();
  if ((pos = _ext.rfind(".")) != std::string::npos) {
    _ext = _ext.substr(pos);
  } else {
    _ext = "";
  }

  // parse jsonp request message.
  if (!query_get("callback").empty()) {
    jsonp = true;
  }
  if (jsonp) {
    jsonp_method = query_get("method");
  }
  return ret;
}

HttpResponseReader *HttpMessage::get_http_response_reader() { return _body; }

HttpServerConn *HttpMessage::connection() { return conn; }

uint8_t HttpMessage::method() {
  if (jsonp && !jsonp_method.empty()) {
    if (jsonp_method == "GET") {
      return HTTP_GET;
    } else if (jsonp_method == "PUT") {
      return HTTP_PUT;
    } else if (jsonp_method == "POST") {
      return HTTP_POST;
    } else if (jsonp_method == "DELETE") {
      return HTTP_DELETE;
    }
  }

  return (uint8_t)_header.method;
}

std::string HttpMessage::method_str() {
  if (jsonp && !jsonp_method.empty()) {
    return jsonp_method;
  }

  if (is_http_get()) {
    return "GET";
  }
  if (is_http_put()) {
    return "PUT";
  }
  if (is_http_post()) {
    return "POST";
  }
  if (is_http_delete()) {
    return "DELETE";
  }
  if (is_http_options()) {
    return "OPTIONS";
  }

  return "OTHER";
}

std::string HttpMessage::uri() {
  std::string uri_ = _uri->get_schema();
  if (uri_.empty()) {
    uri_ += "http";
  }
  uri_ += "://";

  uri_ += host();
  uri_ += path();

  return uri_;
}

int HttpMessage::parse_rest_id(std::string pattern) {
  std::string p = _uri->get_path();
  if (p.length() <= pattern.length()) {
    return -1;
  }

  std::string id = p.substr((int)pattern.length());
  if (!id.empty()) {
    return ::atoi(id.c_str());
  }

  return -1;
}

int HttpMessage::enter_infinite_chunked() {
  int ret = COCO_SUCCESS;

  if (infinite_chunked) {
    return ret;
  }

  if (is_chunked() || content_length() != -1) {
    ret = ERROR_HTTP_DATA_INVALID;
    coco_error("infinite chunkted not supported in specified codec. ret=%d",
               ret);
    return ret;
  }

  infinite_chunked = true;

  return ret;
}

int HttpMessage::body_read_all(std::string &body) {
  int ret = COCO_SUCCESS;

  // cache to read.
  char *buf = new char[HTTP_READ_CACHE_BYTES];
  CocoAutoFreeA(char, buf);

  // whatever, read util EOF.
  while (!_body->eof()) {
    int nb_read = 0;
    if ((ret = _body->Read(buf, HTTP_READ_CACHE_BYTES, &nb_read)) !=
        COCO_SUCCESS) {
      return ret;
    }

    if (nb_read > 0) {
      body.append(buf, nb_read);
    }
  }

  return ret;
}

HttpResponseReader *HttpMessage::body_reader() { return _body; }

void HttpMessage::set_content_length(int64_t len) {
  _header.content_length = len;
}

int64_t HttpMessage::content_length() { return _header.content_length; }

std::string HttpMessage::query_get(std::string key) {
  std::string v;

  if (_query.find(key) != _query.end()) {
    v = _query[key];
  }

  return v;
}

int HttpMessage::request_header_count() { return (int)_headers.size(); }

std::string HttpMessage::request_header_key_at(int index) {
  assert(index < request_header_count());
  HttpHeaderField item = _headers[index];
  return item.first;
}

std::string HttpMessage::request_header_value_at(int index) {
  assert(index < request_header_count());
  HttpHeaderField item = _headers[index];
  return item.second;
}

std::string HttpMessage::get_request_header(std::string name) {
  std::vector<HttpHeaderField>::iterator it;

  for (it = _headers.begin(); it != _headers.end(); ++it) {
    HttpHeaderField &elem = *it;
    std::string key = elem.first;
    std::string value = elem.second;
    if (key == name) {
      return value;
    }
  }

  return "";
}

bool HttpMessage::is_jsonp() { return jsonp; }

IHttpHandler::IHttpHandler() { entry = nullptr; }

IHttpHandler::~IHttpHandler() {}

bool IHttpHandler::is_not_found() { return false; }

HttpRedirectHandler::HttpRedirectHandler(std::string u, int c) {
  url = u;
  code = c;
}

HttpRedirectHandler::~HttpRedirectHandler() {}

int HttpRedirectHandler::serve_http(HttpResponseWriter *w, HttpMessage *r) {
  int ret = COCO_SUCCESS;

  std::string location = url;
  if (!r->query().empty()) {
    location += "?" + r->query();
  }

  std::string msg = "Redirect to" + location;

  w->header()->set_content_type("text/plain; charset=utf-8");
  w->header()->set_content_length(msg.length());
  w->header()->set("Location", location);
  w->WriteHeader(code);

  w->Write((char *)msg.data(), (int)msg.length());
  w->final_request();

  coco_info("redirect to %s.", location.c_str());
  return ret;
}

HttpNotFoundHandler::HttpNotFoundHandler() {}

HttpNotFoundHandler::~HttpNotFoundHandler() {}

bool HttpNotFoundHandler::is_not_found() { return true; }

int HttpNotFoundHandler::serve_http(HttpResponseWriter *w, HttpMessage *r) {
  return go_http_error(w, CONSTS_HTTP_NotFound);
}

HttpMuxEntry::HttpMuxEntry() {
  enabled = true;
  explicit_match = false;
  handler = nullptr;
}

HttpMuxEntry::~HttpMuxEntry() { coco_freep(handler); }

HttpServeMux::HttpServeMux() {}

HttpServeMux::~HttpServeMux() {
  std::map<std::string, HttpMuxEntry *>::iterator it;
  for (it = entries.begin(); it != entries.end(); ++it) {
    HttpMuxEntry *entry = it->second;
    coco_freep(entry);
  }
  entries.clear();

  vhosts.clear();
}

int HttpServeMux::initialize() {
  int ret = COCO_SUCCESS;
  // TODO: FIXME: implements it.
  return ret;
}

void HttpServeMux::remove_entry(std::string pattern) {
  if (entries.find(pattern) == entries.end()) {
    return;
  }
  coco_trace("remove inactive mux patten %s", pattern.c_str());
  HttpMuxEntry *exists = entries[pattern];
  entries.erase(pattern);
  coco_freep(exists);
}

int HttpServeMux::handle(std::string pattern, IHttpHandler *handler) {
  int ret = COCO_SUCCESS;

  assert(handler);

  if (pattern.empty()) {
    ret = ERROR_HTTP_PATTERN_EMPTY;
    coco_error("http: empty pattern. ret=%d", ret);
    return ret;
  }

  if (entries.find(pattern) != entries.end()) {
    HttpMuxEntry *exists = entries[pattern];
    if (exists->explicit_match) {
      ret = ERROR_HTTP_PATTERN_DUPLICATED;
      coco_error("http: multiple registrations for %s. ret=%d", pattern.c_str(),
                 ret);
      return ret;
    }
  }

  std::string vhost = pattern;
  if (pattern.at(0) != '/') {
    if (pattern.find("/") != std::string::npos) {
      vhost = pattern.substr(0, pattern.find("/"));
    }
    vhosts[vhost] = handler;
  }

  if (true) {
    HttpMuxEntry *entry = new HttpMuxEntry();
    entry->explicit_match = true;
    entry->handler = handler;
    entry->pattern = pattern;
    entry->handler->entry = entry;

    if (entries.find(pattern) != entries.end()) {
      HttpMuxEntry *exists = entries[pattern];
      coco_freep(exists);
    }
    entries[pattern] = entry;
  }

  // Helpful behavior:
  // If pattern is /tree/, insert an implicit permanent redirect for /tree.
  // It can be overridden by an explicit registration.
  if (pattern != "/" && !pattern.empty() &&
      pattern.at(pattern.length() - 1) == '/') {
    std::string rpattern = pattern.substr(0, pattern.length() - 1);
    HttpMuxEntry *entry = nullptr;

    // free the exists not explicit entry
    if (entries.find(rpattern) != entries.end()) {
      HttpMuxEntry *exists = entries[rpattern];
      if (!exists->explicit_match) {
        entry = exists;
      }
    }

    // create implicit redirect.
    if (!entry || entry->explicit_match) {
      coco_freep(entry);

      entry = new HttpMuxEntry();
      entry->explicit_match = false;
      entry->handler = new HttpRedirectHandler(pattern, CONSTS_HTTP_Found);
      entry->pattern = pattern;
      entry->handler->entry = entry;

      entries[rpattern] = entry;
    }
  }

  return ret;
}

bool HttpServeMux::can_serve(HttpMessage *r) {
  int ret = COCO_SUCCESS;

  IHttpHandler *h = nullptr;
  if ((ret = find_handler(r, &h)) != COCO_SUCCESS) {
    return false;
  }

  assert(h);
  return !h->is_not_found();
}

int HttpServeMux::serve_http(HttpResponseWriter *w, HttpMessage *r) {
  int ret = COCO_SUCCESS;

  IHttpHandler *handler = nullptr;
  if ((ret = find_handler(r, &handler)) != COCO_SUCCESS) {
    coco_error("find handler failed. ret=%d", ret);
    return ret;
  }

  assert(handler);
  if ((ret = handler->serve_http(w, r)) != COCO_SUCCESS) {
    if (!coco_is_client_gracefully_close(ret)) {
      coco_error("handler serve http failed. ret=%d", ret);
    }
    return ret;
  }

  return ret;
}

int HttpServeMux::find_handler(HttpMessage *r, IHttpHandler **ph) {
  int ret = COCO_SUCCESS;

  // TODO: FIXME: support the path . and ..
  if (r->url().find("..") != std::string::npos) {
    ret = ERROR_HTTP_URL_NOT_CLEAN;
    coco_error("http url not canonical, url=%s. ret=%d", r->url().c_str(), ret);
    return ret;
  }

  if ((ret = match(r, ph)) != COCO_SUCCESS) {
    coco_error("http match handler failed. ret=%d", ret);
    return ret;
  }

  static IHttpHandler *h404 = new HttpNotFoundHandler();
  if (*ph == nullptr) {
    *ph = h404;
  }

  return ret;
}

int HttpServeMux::match(HttpMessage *r, IHttpHandler **ph) {
  int ret = COCO_SUCCESS;

  std::string path = r->path();

  // Host-specific pattern takes precedence over generic ones
  if (!vhosts.empty() && vhosts.find(r->get_host()) != vhosts.end()) {
    path = r->get_host() + path;
  }

  int nb_matched = 0;
  IHttpHandler *h = nullptr;

  std::map<std::string, HttpMuxEntry *>::iterator it;
  for (it = entries.begin(); it != entries.end(); ++it) {
    std::string pattern = it->first;
    HttpMuxEntry *entry = it->second;

    if (!entry->enabled) {
      continue;
    }

    if (!path_match(pattern, path)) {
      continue;
    }

    if (!h || (int)pattern.length() > nb_matched) {
      nb_matched = (int)pattern.length();
      h = entry->handler;
    }
  }

  *ph = h;

  return ret;
}

bool HttpServeMux::path_match(std::string pattern, std::string path) {
  if (pattern.empty()) {
    return false;
  }

  int n = (int)pattern.length();

  // not endswith '/', exactly match.
  if (pattern.at(n - 1) != '/') {
    return pattern == path;
  }

  // endswith '/', match any,
  // for example, '/api/' match '/api/[N]'
  if ((int)path.length() >= n) {
    if (memcmp(pattern.data(), path.data(), n) == 0) {
      return true;
    }
  }

  return false;
}

/* HttpServerConn */
HttpServerConn::HttpServerConn(ConnManager *mgr, TcpConn *conn,
                               HttpServeMux *mux)
    : ConnRoutine(mgr) {
  conn_ = conn;
  _mux = mux;
  _parser = new HttpParser();
  https_ = false;
}

HttpServerConn::HttpServerConn(ConnManager *mgr, SslServer *conn,
                               HttpServeMux *mux)
    : ConnRoutine(mgr) {
  conn_ = conn;
  _mux = mux;
  _parser = new HttpParser();
  https_ = true;
}

HttpServerConn::~HttpServerConn() {
  coco_info("destruct httpserver conn");
  if (conn_) {
    delete conn_;
    conn_ = nullptr;
  }

  if (_parser) {
    delete _parser;
    _parser = nullptr;
  }
}

int HttpServerConn::ProcessRequest(HttpResponseWriter *w, HttpMessage *r) {
  int ret = COCO_SUCCESS;

  coco_trace("HTTP %s %s, content-length=%ld", r->method_str().c_str(),
             r->url().c_str(), r->content_length());

  // use default server mux to serve http request.
  if ((ret = _mux->serve_http(w, r)) != COCO_SUCCESS) {
    if (!coco_is_client_gracefully_close(ret)) {
      coco_error("serve http msg failed. ret=%d", ret);
    }
  }

  return ret;
}

int HttpServerConn::DoCycle() {
  int ret = COCO_SUCCESS;

  // initialize parser
  if ((ret = _parser->initialize(HTTP_REQUEST)) != COCO_SUCCESS) {
    coco_error("api initialize http parser failed. ret=%d", ret);
    return ret;
  }
  conn_->SetRecvTimeout(HTTP_RECV_TIMEOUT_US);

  if (https_) {
    // ssl handshake
    SslServer* ssl = reinterpret_cast<SslServer*>(conn_);
    ret = ssl->Handshake("./server.key", "./server.crt");
    if (ret != COCO_SUCCESS) {
      coco_error("ssl handshake failed");
      return ret;
    }
  }

  // process http messages.
  while (!ShouldTermCycle()) {
    HttpMessage *req = nullptr;
    // get a http message
    if ((ret = _parser->parse_message(conn_, this, &req)) != COCO_SUCCESS) {
      return ret;
    }
    assert(req);

    // always free it in this scope.
    CocoAutoFree(HttpMessage, req);

    // ok, handle http request.
    HttpResponseWriter writer(conn_);
    if ((ret = ProcessRequest(&writer, req)) != COCO_SUCCESS) {
      return ret;
    }

    // read all rest bytes in request body.
    char buf[HTTP_READ_CACHE_BYTES];
    HttpResponseReader *br = req->body_reader();
    while (!br->eof()) {
      if ((ret = br->Read(buf, HTTP_READ_CACHE_BYTES, nullptr)) !=
          COCO_SUCCESS) {
        return ret;
      }
    }

    // donot keep alive, disconnect it.
    if (!req->is_keep_alive()) {
      break;
    }
  }

  return ret;
}

/* HttpServer */
HttpServer::HttpServer(bool https) {
  _l = nullptr;
  _mux = nullptr;
  https_ = https;
  manager = new ConnManager();
}

HttpServer::~HttpServer() {
  if (_l) {
    delete _l;
    _l = nullptr;
  }
  if (manager) {
    delete manager;
    manager = nullptr;
  }
}

int HttpServer::ListenAndServe(std::string local_ip, int local_port,
                               HttpServeMux *mux) {
  _l = ListenTcp(local_ip, local_port);
  if (_l == nullptr) {
    coco_error("create http listen socket failed");
    return -1;
  }
  _mux = mux;
  return 0;
}

int HttpServer::Serve(TcpListener *l, HttpServeMux *mux) {
  _l = l;
  _mux = mux;

  return 0;
}

int HttpServer::Cycle() {
  while (true) {
    manager->Destroy();

    TcpConn *conn_ = _l->Accept();
    if (conn_ == nullptr) {
      coco_error("get null conn");
      continue;
    }
    HttpServerConn *conn = nullptr;
    if (https_) {
      auto ssl = new SslServer(conn_->GetStfd(), conn_);
      conn = new HttpServerConn(manager, ssl, _mux);
    } else {
      conn = new HttpServerConn(manager, conn_, _mux);
    }

    conn->Start();
  }
  return 0;
}

HttpClient::HttpClient() {
  connected = false;
  stfd = nullptr;
  io_ = nullptr;
  parser = nullptr;
  timeout_us = 0;
  port = 0;
  is_https = false;
}

HttpClient::~HttpClient() {
  disconnect();
  coco_freep(parser);
}

int HttpClient::initialize(std::string _h, int p, int64_t t_us,
                           std::string url) {
  int ret = COCO_SUCCESS;

  coco_freep(parser);
  parser = new HttpParser();

  if ((ret = parser->initialize(HTTP_RESPONSE)) != COCO_SUCCESS) {
    coco_error("initialize parser failed. ret=%d", ret);
    return ret;
  }

  host = _h;
  port = p;
  timeout_us = HTTP_CLIENT_TIMEOUT_US;

  set_https_flag(url.find("https") == 0);
  // we just handle the default port when https
  if ((is_https) && (80 == port)) {
    port = 443;
  }
  return ret;
}

int HttpClient::post(std::string path, std::string req, HttpMessage **ppmsg,
                     std::string request_id) {
  *ppmsg = nullptr;

  int ret = COCO_SUCCESS;

  if ((ret = connect()) != COCO_SUCCESS) {
    coco_warn("http post. connect server failed. [host:%s, port:%d]ret=%d",
              host.c_str(), port, ret);
    return ret;
  }

  // send POST request to uri
  // POST %s HTTP/1.1\r\nHost: %s\r\nContent-Length: %d\r\n\r\n%s
  std::stringstream ss;
  ss << "POST " << path << " "
     << "HTTP/1.1" << HTTP_CRLF << "Host: " << host << HTTP_CRLF
     << "Request-Id: " << request_id << HTTP_CRLF << "Connection: Keep-Alive"
     << HTTP_CRLF << "Content-Length: " << std::dec << req.length() << HTTP_CRLF
     << "User-Agent: "
     << "coco" << HTTP_CRLF << "Content-Type: application/json" << HTTP_CRLF
     << HTTP_CRLF << req;

  std::string data = ss.str();
  if ((ret = io_->Write((void *)data.c_str(), data.length(), nullptr)) !=
      COCO_SUCCESS) {
    // disconnect when error.
    disconnect();

    coco_error("http post. write failed. ret=%d", ret);
    return ret;
  }

  HttpMessage *msg = nullptr;
  if ((ret = parser->parse_message(io_, nullptr, &msg)) != COCO_SUCCESS) {
    coco_error("http post. parse response failed. ret=%d", ret);
    return ret;
  }

  assert(msg);
  *ppmsg = msg;
  coco_info("http post. parse response success.");

  return ret;
}

int HttpClient::get(std::string path, std::string req, HttpMessage **ppmsg,
                    std::string request_id) {
  *ppmsg = nullptr;

  int ret = COCO_SUCCESS;

  if ((ret = connect()) != COCO_SUCCESS) {
    coco_warn("http connect server failed. ret=%d", ret);
    return ret;
  }

  // send POST request to uri
  // GET %s HTTP/1.1\r\nHost: %s\r\nContent-Length: %d\r\n\r\n%s
  std::stringstream ss;
  ss << "GET " << path << " "
     << "HTTP/1.1" << HTTP_CRLF << "Host: " << host << HTTP_CRLF
     << "Request-Id: " << request_id << HTTP_CRLF << "Connection: Keep-Alive"
     << HTTP_CRLF << "Content-Length: " << std::dec << req.length() << HTTP_CRLF
     << "User-Agent: "
     << "coco" << HTTP_CRLF << "Content-Type: application/json" << HTTP_CRLF
     << HTTP_CRLF << req;

  std::string data = ss.str();
  if ((ret = io_->Write((void *)data.c_str(), data.length(), nullptr)) !=
      COCO_SUCCESS) {
    // disconnect when error.
    disconnect();
    coco_error("write http get failed. ret=%d", ret);
    return ret;
  }

  HttpMessage *msg = nullptr;
  if ((ret = parser->parse_message(io_, nullptr, &msg)) != COCO_SUCCESS) {
    coco_error("parse http post response failed. ret=%d", ret);
    return ret;
  }
  assert(msg);

  *ppmsg = msg;
  coco_info("parse http get response success.");

  return ret;
}

void HttpClient::disconnect() {
  connected = false;
  coco_freep(conn);
}

int HttpClient::connect() {
  int ret = COCO_SUCCESS;

  if (connected) {
    return ret;
  }

  disconnect();

  // open socket.
  conn = DialTcp(host, port, (int)timeout_us);
  if (conn == nullptr) {
    coco_warn("http client failed, server=%s, port=%d, timeout=%ld",
              host.c_str(), port, timeout_us);
    return -1;
  }
  coco_info("connect to server success. server=%s, port=%d", host.c_str(),
            port);

  assert(!io_);
  io_ =  conn->GetCocoSocket();
  conn->SetRecvTimeout(timeout_us);
  conn->SetSendTimeout(timeout_us);
  connected = true;

  return ret;
}

void HttpClient::set_https_flag(bool b_flag) { is_https = b_flag; }