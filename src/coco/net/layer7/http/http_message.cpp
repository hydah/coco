#include "coco/net/layer7/http/http_message.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "http-parser/http_parser.h"

#include "coco/common/error.hpp"
#include "coco/log/log.hpp"

namespace coco {

// Chunk-size lines and trailer lines longer than this are rejected.
static const size_t kMaxLineBytes = 4096;

// Buffers br up to and including the next LF; *len counts the LF.
static int BufferLine(BufReader *br, size_t *len) {
    size_t from = 0;
    while (true) {
        const char *p = br->Peek();
        size_t n = br->Buffered();
        const void *lf = memchr(p + from, '\n', n - from);
        if (lf != nullptr) {
            *len = (size_t)((const char *)lf - p) + 1;
            return COCO_SUCCESS;
        }
        if (n >= kMaxLineBytes) {
            return ERROR_HTTP_INVALID_CHUNK_HEADER;
        }
        from = n;
        int ret = br->Fill(kMaxLineBytes);
        if (ret != COCO_SUCCESS) {
            return ret;
        }
    }
}

static bool IsBlankLine(const char *p, size_t len) {
    return (len == 1 && p[0] == '\n') || (len == 2 && p[0] == '\r' && p[1] == '\n');
}

void HttpBodyReader::Reset(BufReader *br, Mode mode, int64_t length) {
    br_ = br;
    mode_ = mode;
    remain_ = mode == kLength ? length : 0;
    chunk_crlf_ = false;
    eof_ = mode == kNone || (mode == kLength && length <= 0);
    err_ = COCO_SUCCESS;
    continue_ = nullptr;
}

int HttpBodyReader::ReadChunkHeader() {
    size_t len = 0;
    int ret;
    if (chunk_crlf_) {
        if ((ret = BufferLine(br_, &len)) != COCO_SUCCESS) {
            return ret;
        }
        if (!IsBlankLine(br_->Peek(), len)) {
            return ERROR_HTTP_INVALID_CHUNK_HEADER;
        }
        br_->Consume(len);
        chunk_crlf_ = false;
    }

    if ((ret = BufferLine(br_, &len)) != COCO_SUCCESS) {
        return ret;
    }
    const char *p = br_->Peek();
    int64_t size = 0;
    size_t digits = 0;
    for (; digits < len; ++digits) {
        char c = p[digits];
        int v = (c >= '0' && c <= '9')   ? c - '0'
                : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                         : -1;
        if (v < 0) {
            break;
        }
        if (digits >= 15) {
            return ERROR_HTTP_INVALID_CHUNK_HEADER;
        }
        size = size * 16 + v;
    }
    // Anything after the size must be chunk extensions or whitespace.
    char next = digits < len ? p[digits] : '\n';
    if (digits == 0 ||
        (next != ';' && next != ' ' && next != '\t' && next != '\r' && next != '\n')) {
        return ERROR_HTTP_INVALID_CHUNK_HEADER;
    }
    br_->Consume(len);

    if (size > 0) {
        remain_ = size;
        return COCO_SUCCESS;
    }

    // The last chunk, then trailer fields up to a blank line; the trailers are dropped.
    while (true) {
        if ((ret = BufferLine(br_, &len)) != COCO_SUCCESS) {
            return ret;
        }
        bool blank = IsBlankLine(br_->Peek(), len);
        br_->Consume(len);
        if (blank) {
            break;
        }
    }
    eof_ = true;
    return COCO_SUCCESS;
}

int HttpBodyReader::Read(void *buf, size_t size, ssize_t *nread) {
    if (nread) {
        *nread = 0;
    }
    if (err_ != COCO_SUCCESS) {
        return err_;
    }
    if (eof_) {
        return ERROR_HTTP_BODY_EOF;
    }
    if (size == 0) {
        return COCO_SUCCESS;
    }

    if (continue_ != nullptr) {
        static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
        IoWriter *w = continue_;
        continue_ = nullptr;
        if ((err_ = w->Write((void *)kContinue, sizeof(kContinue) - 1, nullptr)) != COCO_SUCCESS) {
            return err_;
        }
    }

    if (mode_ == kChunked && remain_ == 0) {
        if ((err_ = ReadChunkHeader()) != COCO_SUCCESS) {
            return err_;
        }
        if (eof_) {
            return ERROR_HTTP_BODY_EOF;
        }
    }

    if (mode_ != kUntilEof) {
        size = (size_t)std::min<int64_t>((int64_t)size, remain_);
    }
    ssize_t n = 0;
    int ret = br_->Read(buf, size, &n);
    if (ret != COCO_SUCCESS) {
        if (mode_ == kUntilEof && ret == ERROR_SOCKET_READ && n == 0) {
            eof_ = true;
            return ERROR_HTTP_BODY_EOF;
        }
        err_ = ret;
        return ret;
    }

    if (mode_ == kLength) {
        remain_ -= n;
        eof_ = remain_ == 0;
    } else if (mode_ == kChunked) {
        remain_ -= n;
        chunk_crlf_ = remain_ == 0;
    }
    if (nread) {
        *nread = n;
    }
    return COCO_SUCCESS;
}

int HttpBodyReader::ReadAll(std::string *body) {
    if (mode_ == kLength) {
        // A peer's Content-Length is not trusted for more than a modest reservation.
        body->reserve(body->size() + (size_t)std::min<int64_t>(remain_, 1 << 20));
    }
    while (!eof_) {
        size_t chunk = mode_ == kLength ? (size_t)std::min<int64_t>(remain_, 64 * 1024)
                                        : (size_t)16 * 1024;
        size_t old = body->size();
        body->resize(old + chunk);
        ssize_t n = 0;
        int ret = Read(&(*body)[old], chunk, &n);
        body->resize(old + (size_t)n);
        if (ret == ERROR_HTTP_BODY_EOF) {
            break;
        }
        if (ret != COCO_SUCCESS) {
            return ret;
        }
    }
    return COCO_SUCCESS;
}

int HttpBodyReader::Discard(int64_t limit) {
    char buf[HTTP_READ_CACHE_BYTES];
    int64_t total = 0;
    while (!eof_) {
        ssize_t n = 0;
        int ret = Read(buf, sizeof(buf), &n);
        if (ret == ERROR_HTTP_BODY_EOF) {
            break;
        }
        if (ret != COCO_SUCCESS) {
            return ret;
        }
        if ((total += n) > limit) {
            return ERROR_HTTP_CONTENT_LENGTH;
        }
    }
    return COCO_SUCCESS;
}

bool HttpBodyReader::DiscardBuffered() {
    if (!eof_ && err_ == COCO_SUCCESS && mode_ == kLength && br_->Buffered() >= (size_t)remain_) {
        br_->Consume((size_t)remain_);
        remain_ = 0;
        eof_ = true;
    }
    return eof_;
}

HttpRequest::HttpRequest(const std::string &method, const std::string &url,
                         const std::string &body)
    : method(method), url(url), send_body_(body) {}

void HttpRequest::Reset() {
    method.clear();
    url.clear();
    path.clear();
    raw_query.clear();
    header.Clear();
    host.clear();
    content_length = 0;
    close = false;
    upgrade_ = false;
    query_parsed_ = false;
    path_values_.clear();
}

const HttpValues &HttpRequest::Query() {
    if (!query_parsed_) {
        query_ = HttpValues::Parse(raw_query);
        query_parsed_ = true;
    }
    return query_;
}

const std::string &HttpRequest::PathValue(const std::string &name) const {
    for (auto &kv : path_values_) {
        if (kv.first == name) {
            return kv.second;
        }
    }
    static const std::string kEmpty;
    return kEmpty;
}

void HttpRequest::SetPathValue(const std::string &name, const std::string &value) {
    for (auto &kv : path_values_) {
        if (kv.first == name) {
            kv.second = value;
            return;
        }
    }
    path_values_.push_back(std::make_pair(name, value));
}

// Finds the blank line that ends a header block starting at p; returns the block's length
// including that line, or 0 when it is not complete yet. Scanning starts at from.
static size_t FindHeaderEnd(const char *p, size_t n, size_t from) {
    size_t i = from;
    while (i < n) {
        const char *lf = (const char *)memchr(p + i, '\n', n - i);
        if (lf == nullptr) {
            return 0;
        }
        size_t j = (size_t)(lf - p) + 1;
        if (j < n && p[j] == '\n') {
            return j + 1;
        }
        if (j + 1 < n && p[j] == '\r' && p[j + 1] == '\n') {
            return j + 2;
        }
        i = j;
    }
    return 0;
}

static int ReadHeaderBlock(BufReader *br, size_t max_bytes, size_t *len) {
    size_t from = 0;
    while (true) {
        // RFC 7230 3.5: ignore empty lines before the start line.
        while (from == 0 && br->Buffered() > 0 &&
               (br->Peek()[0] == '\r' || br->Peek()[0] == '\n')) {
            br->Consume(1);
        }
        size_t n = br->Buffered();
        if (n > 0) {
            size_t end = FindHeaderEnd(br->Peek(), std::min(n, max_bytes), from);
            if (end > 0) {
                *len = end;
                return COCO_SUCCESS;
            }
            from = n >= 3 ? n - 3 : 0;
        }
        if (n >= max_bytes) {
            return ERROR_HTTP_HEADER_TOO_LARGE;
        }
        int ret = br->Fill(max_bytes);
        if (ret == ERROR_READER_BUFFER_OVERFLOW) {
            return ERROR_HTTP_HEADER_TOO_LARGE;
        }
        if (ret != COCO_SUCCESS) {
            return ret;
        }
    }
}

// Runs http-parser over exactly one header block and collects what it reports.
class HttpRequestParser {
 public:
    HttpHeader *header = nullptr;
    std::string *url = nullptr;
    std::string *reason = nullptr;
    // 0 before any field, 1 inside a field name, 2 inside a value.
    int field_state = 0;
    bool complete = false;

    unsigned method = 0;
    unsigned status_code = 0;
    unsigned short major = 1;
    unsigned short minor = 1;
    bool chunked = false;
    bool has_length = false;
    int64_t length = 0;
    bool upgrade = false;
    bool keep_alive = true;

    int Parse(enum http_parser_type type, const char *p, size_t n) {
        http_parser parser;
        http_parser_init(&parser, type);
        parser.data = this;
        size_t parsed = http_parser_execute(&parser, Settings(), p, n);
        enum http_errno err = HTTP_PARSER_ERRNO(&parser);
        if (err == HPE_HEADER_OVERFLOW) {
            return ERROR_HTTP_HEADER_TOO_LARGE;
        }
        if (err != HPE_OK || parsed != n || !complete) {
            coco_warn("http: bad header, %s", http_errno_description(err));
            return ERROR_HTTP_PARSE_HEADER;
        }
        return COCO_SUCCESS;
    }

    static int ParseRequest(BufReader *br, size_t max_header_bytes, HttpRequest *req);
    static int ParseResponse(BufReader *br, size_t max_header_bytes, const std::string &method,
                             HttpResponse *resp);

 private:
    static const http_parser_settings *Settings() {
        static http_parser_settings s = MakeSettings();
        return &s;
    }

    static http_parser_settings MakeSettings() {
        http_parser_settings s;
        memset(&s, 0, sizeof(s));
        s.on_url = OnUrl;
        s.on_status = OnStatus;
        s.on_header_field = OnHeaderField;
        s.on_header_value = OnHeaderValue;
        s.on_headers_complete = OnHeadersComplete;
        return s;
    }

    static int OnUrl(http_parser *p, const char *at, size_t len) {
        HttpRequestParser *self = (HttpRequestParser *)p->data;
        if (self->url) {
            self->url->append(at, len);
        }
        return 0;
    }

    static int OnStatus(http_parser *p, const char *at, size_t len) {
        HttpRequestParser *self = (HttpRequestParser *)p->data;
        if (self->reason) {
            self->reason->append(at, len);
        }
        return 0;
    }

    static int OnHeaderField(http_parser *p, const char *at, size_t len) {
        HttpRequestParser *self = (HttpRequestParser *)p->data;
        std::vector<HttpHeader::Field> &fields = self->header->fields_;
        if (self->field_state != 1) {
            fields.push_back(HttpHeader::Field());
            self->field_state = 1;
        }
        fields.back().first.append(at, len);
        return 0;
    }

    static int OnHeaderValue(http_parser *p, const char *at, size_t len) {
        HttpRequestParser *self = (HttpRequestParser *)p->data;
        self->header->fields_.back().second.append(at, len);
        self->field_state = 2;
        return 0;
    }

    static int OnHeadersComplete(http_parser *p) {
        HttpRequestParser *self = (HttpRequestParser *)p->data;
        self->complete = true;
        self->method = p->method;
        self->status_code = p->status_code;
        self->major = p->http_major;
        self->minor = p->http_minor;
        self->chunked = (p->flags & F_CHUNKED) != 0;
        self->has_length = (p->flags & F_CONTENTLENGTH) != 0 && p->content_length != ULLONG_MAX;
        self->length = self->has_length ? (int64_t)p->content_length : 0;
        self->upgrade = p->upgrade != 0;
        self->keep_alive = http_should_keep_alive(p) != 0;
        return 0;
    }
};

int HttpRequestParser::ParseRequest(BufReader *br, size_t max_header_bytes, HttpRequest *req) {
    size_t len = 0;
    int ret = ReadHeaderBlock(br, max_header_bytes, &len);
    if (ret != COCO_SUCCESS) {
        return ret;
    }

    req->Reset();
    HttpRequestParser hp;
    hp.header = &req->header;
    hp.url = &req->url;
    ret = hp.Parse(HTTP_REQUEST, br->Peek(), len);
    br->Consume(len);
    if (ret != COCO_SUCCESS) {
        return ret;
    }

    req->method = http_method_str((enum http_method)hp.method);
    req->proto_major = hp.major;
    req->proto_minor = hp.minor;
    req->proto = "HTTP/" + std::to_string(hp.major) + "." + std::to_string(hp.minor);
    req->close = !hp.keep_alive;
    req->upgrade_ = hp.upgrade;
    req->host = req->header.Get(HttpHeaderHost);

    const std::string &url = req->url;
    if (!url.empty() && url[0] == '/') {
        size_t q = url.find('?');
        if (q != std::string::npos) {
            req->raw_query.assign(url, q + 1, std::string::npos);
        }
        if (!HttpPathUnescape(url.substr(0, q), &req->path)) {
            return ERROR_HTTP_PARSE_URI;
        }
    } else if (url == "*") {
        req->path = url;
    } else {
        // absolute-form, or authority-form for CONNECT.
        bool connect = hp.method == HTTP_CONNECT;
        http_parser_url u;
        http_parser_url_init(&u);
        if (http_parser_parse_url(url.data(), url.size(), connect, &u) != 0) {
            return ERROR_HTTP_PARSE_URI;
        }
        auto field = [&](http_parser_url_fields f) {
            return (u.field_set & (1 << f)) ? url.substr(u.field_data[f].off, u.field_data[f].len)
                                            : std::string();
        };
        req->host = connect ? url : field(UF_HOST);
        req->raw_query = field(UF_QUERY);
        if (!HttpPathUnescape(field(UF_PATH), &req->path)) {
            return ERROR_HTTP_PARSE_URI;
        }
        if (req->path.empty() && !connect) {
            req->path = "/";
        }
    }

    if (hp.upgrade) {
        req->content_length = 0;
        req->body.Reset(br, HttpBodyReader::kNone);
    } else if (hp.chunked) {
        req->content_length = -1;
        req->body.Reset(br, HttpBodyReader::kChunked);
    } else {
        req->content_length = hp.length;
        req->body.Reset(br, hp.length > 0 ? HttpBodyReader::kLength : HttpBodyReader::kNone,
                        hp.length);
    }
    return COCO_SUCCESS;
}

int HttpRequestParser::ParseResponse(BufReader *br, size_t max_header_bytes,
                                     const std::string &method, HttpResponse *resp) {
    size_t len = 0;
    int ret = ReadHeaderBlock(br, max_header_bytes, &len);
    if (ret != COCO_SUCCESS) {
        return ret;
    }

    resp->header.Clear();
    std::string reason;
    HttpRequestParser hp;
    hp.header = &resp->header;
    hp.reason = &reason;
    ret = hp.Parse(HTTP_RESPONSE, br->Peek(), len);
    br->Consume(len);
    if (ret != COCO_SUCCESS) {
        return ret;
    }

    int code = (int)hp.status_code;
    resp->status_code = code;
    resp->status = std::to_string(code) + " " + reason;
    resp->proto_major = hp.major;
    resp->proto_minor = hp.minor;
    resp->proto = "HTTP/" + std::to_string(hp.major) + "." + std::to_string(hp.minor);
    resp->close = !hp.keep_alive;

    if (method == HttpMethodHead || !HttpBodyAllowedForStatus(code)) {
        resp->content_length = hp.has_length ? hp.length : 0;
        resp->body.Reset(br, HttpBodyReader::kNone);
    } else if (hp.chunked) {
        resp->content_length = -1;
        resp->body.Reset(br, HttpBodyReader::kChunked);
    } else if (hp.has_length) {
        resp->content_length = hp.length;
        resp->body.Reset(br, HttpBodyReader::kLength, hp.length);
    } else {
        resp->content_length = -1;
        resp->close = true;
        resp->body.Reset(br, HttpBodyReader::kUntilEof);
    }
    return COCO_SUCCESS;
}

int ReadHttpRequest(BufReader *br, size_t max_header_bytes, HttpRequest *req) {
    return HttpRequestParser::ParseRequest(br, max_header_bytes, req);
}

int ReadHttpResponse(BufReader *br, size_t max_header_bytes, const std::string &method,
                     HttpResponse *resp) {
    return HttpRequestParser::ParseResponse(br, max_header_bytes, method, resp);
}

}  // namespace coco
