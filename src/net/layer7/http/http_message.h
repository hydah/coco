#pragma once
#include <stdint.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "net/layer4/coco_layer4.hpp"
#include "net/layer7/http/http_basic.h"
#include "utils/bufio.hpp"

// Reads one message body from the connection's BufReader, framed by Content-Length, by
// chunked encoding, or by the connection closing. Bytes past the body stay buffered for
// the next message.
class HttpBodyReader : public IoReader {
 public:
    enum Mode { kNone, kLength, kChunked, kUntilEof };

    void Reset(BufReader *br, Mode mode, int64_t length = 0);

    // COCO_SUCCESS with *nread > 0, or ERROR_HTTP_BODY_EOF once the body is complete.
    int Read(void *buf, size_t size, ssize_t *nread) override;
    // Appends the rest of the body to *body.
    int ReadAll(std::string *body);
    bool Eof() const { return eof_; }

    // Reads and drops the rest of the body, failing if it is longer than limit.
    int Discard(int64_t limit);
    // Drops the rest of the body if it is already buffered, without any I/O; returns
    // whether the body is complete now.
    bool DiscardBuffered();

    // The server's "100 Continue" goes out through w on the first read (RFC 7231 5.1.1).
    void SetContinue(IoWriter *w) { continue_ = w; }
    bool ContinuePending() const { return continue_ != nullptr; }
    void CancelContinue() { continue_ = nullptr; }

 private:
    int ReadChunkHeader();

    BufReader *br_ = nullptr;
    Mode mode_ = kNone;
    // kLength: bytes left in the body. kChunked: bytes left in the current chunk.
    int64_t remain_ = 0;
    // kChunked: the CRLF after the current chunk's data is still to be read.
    bool chunk_crlf_ = false;
    bool eof_ = true;
    // The first read error; the body is unusable afterwards.
    int err_ = 0;
    IoWriter *continue_ = nullptr;
};

// An HTTP request. On a server it is what the peer sent; the fields are Go's Request
// fields. For HttpClient::Do, build one with the constructor and set header as needed.
class HttpRequest {
 public:
    HttpRequest() = default;
    // url is absolute, "http://host[:port]/path?query" or https://.
    HttpRequest(const std::string &method, const std::string &url, const std::string &body = "");

    std::string method = "GET";
    // Server: the request-target as sent, e.g. "/a%20b?x=1". Client: the absolute URL.
    std::string url;
    // Server: the unescaped path, "/a b", and the query without '?', "x=1".
    std::string path;
    std::string raw_query;
    std::string proto = "HTTP/1.1";
    int proto_major = 1;
    int proto_minor = 1;
    HttpHeader header;
    // Server: the Host header. Client: overrides the URL's host in the Host header.
    std::string host;
    // Server: the declared body size, -1 when chunked.
    int64_t content_length = 0;
    // Server: the connection closes after the response. Client: ask the server to close.
    bool close = false;
    // Server: the peer's "ip:port".
    std::string remote_addr;
    // Server: the request body.
    HttpBodyReader body;

    bool ProtoAtLeast(int major, int minor) const {
        return proto_major > major || (proto_major == major && proto_minor >= minor);
    }
    // An Upgrade (e.g. WebSocket) or CONNECT request; the connection is closed after the
    // response unless the handler hijacks it.
    bool IsUpgrade() const { return upgrade_; }
    // raw_query parsed on first use.
    const HttpValues &Query();
    // The value a {name} wildcard in the matched HttpServeMux pattern captured.
    const std::string &PathValue(const std::string &name) const;
    void SetPathValue(const std::string &name, const std::string &value);

    // Client: the body to send.
    void SetBody(const std::string &body) { send_body_ = body; }
    const std::string &SendBody() const { return send_body_; }

 private:
    friend class HttpRequestParser;

    void Reset();

    bool upgrade_ = false;
    bool query_parsed_ = false;
    HttpValues query_;
    std::vector<std::pair<std::string, std::string>> path_values_;
    std::string send_body_;
};

class HttpClientConn;
class HttpConnPool;

// A response HttpClient received. Destroying it returns the connection to the client's
// pool when the body was read to the end (or is small enough to be already buffered),
// and closes it otherwise.
class HttpResponse {
 public:
    HttpResponse();
    ~HttpResponse();

    HttpResponse(const HttpResponse &) = delete;
    HttpResponse &operator=(const HttpResponse &) = delete;

    int status_code = 0;
    // e.g. "200 OK".
    std::string status;
    std::string proto;
    int proto_major = 1;
    int proto_minor = 1;
    HttpHeader header;
    // -1 when unknown: chunked, or delimited by the connection closing.
    int64_t content_length = -1;
    // The server closes the connection after this response.
    bool close = false;
    HttpBodyReader body;

    // After 101 Switching Protocols the connection speaks the new protocol: write to
    // Conn() and read through Reader(), which holds bytes that arrived with the response.
    // Both stay owned by the response. nullptr once closed.
    StreamConn *Conn();
    BufReader *Reader();
    // Closes the connection now.
    void Close();

 private:
    friend class HttpClient;
    friend class HttpRequestParser;

    std::unique_ptr<HttpClientConn> conn_;
    std::weak_ptr<HttpConnPool> pool_;
};

// Reads the next request's header from br and prepares req->body. Leading blank lines are
// skipped. ERROR_HTTP_HEADER_TOO_LARGE past max_header_bytes, ERROR_HTTP_PARSE_HEADER or
// ERROR_HTTP_PARSE_URI for a malformed request, otherwise br's read error.
int ReadHttpRequest(BufReader *br, size_t max_header_bytes, HttpRequest *req);
// Reads a response header for a request with method and prepares resp->body.
int ReadHttpResponse(BufReader *br, size_t max_header_bytes, const std::string &method,
                     HttpResponse *resp);
