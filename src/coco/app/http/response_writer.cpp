#include "coco/app/http/response_writer.hpp"

#include <stdlib.h>
#include <sys/uio.h>

#include "coco/common/error.hpp"
#include "coco/app/http/codec/response.hpp"
#include "coco/log/log.hpp"

namespace coco {

// Output is coalesced up to this size; Go uses a 4KB bufio.Writer too.
static const size_t kWriteBufferBytes = 4096;

HttpResponseWriter::HttpResponseWriter(StreamConn *conn, BufReader *br) : conn_(conn), br_(br) {
    out_.reserve(kWriteBufferBytes);
}

void HttpResponseWriter::Reset(HttpRequest *r) {
    req_ = r;
    header_.Clear();
    status_ = HttpStatusOK;
    wrote_header_ = false;
    committed_ = false;
    chunked_ = false;
    body_allowed_ = true;
    is_head_ = r->method == HttpMethodHead;
    close_ = r->close || r->IsUpgrade();
    hijacked_ = false;
    content_length_ = -1;
    written_ = 0;
    err_ = COCO_SUCCESS;
    pending_.clear();
    out_.clear();
}

void HttpResponseWriter::WriteHeader(int code) {
    if (hijacked_) {
        coco_warn("http: WriteHeader on a hijacked connection");
        return;
    }
    if (wrote_header_) {
        coco_warn("http: superfluous WriteHeader(%d), already %d", code, status_);
        return;
    }
    if (code < 100 || code > 999) {
        coco_error("http: invalid status code %d, sending 500", code);
        code = HttpStatusInternalServerError;
    }
    wrote_header_ = true;
    status_ = code;
    body_allowed_ = HttpBodyAllowedForStatus(code);

    const std::string &cl = header_.Get(HttpHeaderContentLength);
    if (!cl.empty()) {
        char *end = nullptr;
        long long v = strtoll(cl.c_str(), &end, 10);
        if (v >= 0 && end && *end == '\0') {
            content_length_ = v;
        } else {
            coco_warn("http: invalid Content-Length %s, ignored", cl.c_str());
            header_.Del(HttpHeaderContentLength);
        }
    }
}

int HttpResponseWriter::Write(const void *data, size_t size) {
    if (hijacked_) {
        return ERROR_HTTP_HIJACKED;
    }
    if (err_ != COCO_SUCCESS) {
        return err_;
    }
    if (!wrote_header_) {
        WriteHeader(HttpStatusOK);
    }
    if (size == 0) {
        return COCO_SUCCESS;
    }
    if (!body_allowed_) {
        return ERROR_HTTP_BODY_NOT_ALLOWED;
    }
    if (content_length_ >= 0 && written_ + (int64_t)size > content_length_) {
        return ERROR_HTTP_CONTENT_LENGTH;
    }
    written_ += (int64_t)size;
    if (is_head_) {
        return COCO_SUCCESS;
    }

    if (!committed_) {
        if (pending_.size() + size <= kWriteBufferBytes) {
            pending_.append((const char *)data, size);
            return COCO_SUCCESS;
        }
        Commit(false);
    }
    return WriteBody((const char *)data, size);
}

int HttpResponseWriter::WriteBody(const char *data, size_t size) {
    if (chunked_) {
        char hex[kHttpChunkHeaderMax];
        size_t hn = HttpFormatChunkHeader(size, hex);
        if (out_.size() + hn + size + 2 <= kWriteBufferBytes) {
            out_.append(hex, hn);
            out_.append(data, size);
            out_.append(HTTP_CRLF, 2);
            return COCO_SUCCESS;
        }
        iovec iov[4] = {{(void *)out_.data(), out_.size()},
                        {hex, hn},
                        {(void *)data, size},
                        {(void *)HTTP_CRLF, 2}};
        return WriteIovs(iov, 4);
    }

    if (out_.size() + size <= kWriteBufferBytes) {
        out_.append(data, size);
        return COCO_SUCCESS;
    }
    iovec iov[2] = {{(void *)out_.data(), out_.size()}, {(void *)data, size}};
    return WriteIovs(iov, 2);
}

int HttpResponseWriter::WriteIovs(iovec *iov, int n) {
    // Empty entries are dropped: a zero-byte write reads as a failure at the socket.
    iovec v[4];
    int cnt = 0;
    for (int i = 0; i < n; ++i) {
        if (iov[i].iov_len > 0) {
            v[cnt++] = iov[i];
        }
    }
    if (cnt > 0 && (err_ = conn_->Writev(v, cnt, nullptr)) != COCO_SUCCESS) {
        return err_;
    }
    out_.clear();
    return COCO_SUCCESS;
}

int HttpResponseWriter::FlushOut() {
    if (err_ != COCO_SUCCESS || out_.empty()) {
        return err_;
    }
    if ((err_ = conn_->Write((void *)out_.data(), out_.size(), nullptr)) != COCO_SUCCESS) {
        return err_;
    }
    out_.clear();
    return COCO_SUCCESS;
}

void HttpResponseWriter::Commit(bool final) {
    committed_ = true;

    if (content_length_ < 0 && final && body_allowed_ && (!is_head_ || written_ > 0)) {
        content_length_ = written_;
        header_.Set(HttpHeaderContentLength, std::to_string(written_));
    }
    if (!body_allowed_) {
        header_.Del(HttpHeaderTransferEncoding);
    } else if (content_length_ < 0 && !is_head_) {
        if (req_->ProtoAtLeast(1, 1)) {
            chunked_ = true;
            header_.Set(HttpHeaderTransferEncoding, "chunked");
        } else {
            close_ = true;
        }
    }

    if (body_allowed_ && !pending_.empty() && !header_.Has(HttpHeaderContentType)) {
        header_.Set(HttpHeaderContentType, HttpDetectContentType(pending_.data(), pending_.size()));
    }
    if (!header_.Has(HttpHeaderDate)) {
        header_.Set(HttpHeaderDate, HttpDate());
    }

    // The client waits for "100 Continue" before sending the body; now it never comes,
    // so the unread body cannot be skipped and the connection must end.
    if (req_->body.ContinuePending()) {
        req_->body.CancelContinue();
        if (!req_->body.Eof()) {
            close_ = true;
        }
    }
    if (header_.HasToken(HttpHeaderConnection, "close")) {
        close_ = true;
    }
    if (close_) {
        if (!header_.HasToken(HttpHeaderConnection, "close")) {
            header_.Set(HttpHeaderConnection, "close");
        }
    } else if (!req_->ProtoAtLeast(1, 1)) {
        header_.Set(HttpHeaderConnection, "keep-alive");
    }

    HttpAppendStatusLine(status_, &out_);
    header_.WriteTo(&out_);
    out_.append(HTTP_CRLF, 2);

    if (!pending_.empty()) {
        if (chunked_) {
            char hex[kHttpChunkHeaderMax];
            out_.append(hex, HttpFormatChunkHeader(pending_.size(), hex));
            out_.append(pending_);
            out_.append(HTTP_CRLF, 2);
        } else {
            out_.append(pending_);
        }
        pending_.clear();
    }
}

int HttpResponseWriter::Flush() {
    if (hijacked_) {
        return ERROR_HTTP_HIJACKED;
    }
    if (!wrote_header_) {
        WriteHeader(HttpStatusOK);
    }
    if (!committed_) {
        Commit(false);
    }
    return FlushOut();
}

int HttpResponseWriter::Finish() {
    if (hijacked_) {
        return COCO_SUCCESS;
    }
    if (!wrote_header_) {
        WriteHeader(HttpStatusOK);
    }
    if (!committed_) {
        Commit(true);
    }
    if (chunked_) {
        out_.append("0\r\n\r\n", 5);
    }
    // A short body leaves the peer waiting for bytes that never come.
    if (content_length_ >= 0 && written_ < content_length_ && body_allowed_ && !is_head_) {
        coco_warn("http: handler wrote %lld of %lld bytes", (long long)written_,
                  (long long)content_length_);
        close_ = true;
    }
    return FlushOut();
}

int HttpResponseWriter::Hijack(StreamConn **conn, BufReader **br) {
    if (hijacked_) {
        return ERROR_HTTP_HIJACKED;
    }
    int ret = FlushOut();
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    hijacked_ = true;
    *conn = conn_;
    *br = br_;
    return COCO_SUCCESS;
}

}  // namespace coco
