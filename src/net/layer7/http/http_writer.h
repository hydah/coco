#pragma once
#include <stdint.h>

#include <string>

#include "net/layer4/coco_layer4.hpp"
#include "net/layer7/http/http_basic.h"
#include "net/layer7/http/http_message.h"
#include "utils/bufio.hpp"

// Builds the response to one request, with Go's ResponseWriter semantics:
//
//   - Header() may be changed until the first Write() or WriteHeader().
//   - Write() without WriteHeader() sends 200.
//   - Writes are buffered. A handler that writes less than the buffer and returns gets a
//     Content-Length response sent with a single write; a larger body without a declared
//     Content-Length is sent chunked (or delimited by closing, for HTTP/1.0).
//   - Content-Type is sniffed from the first bytes when not set; Date is added.
class HttpResponseWriter {
 public:
    HttpResponseWriter(StreamConn *conn, BufReader *br);

    HttpResponseWriter(const HttpResponseWriter &) = delete;
    HttpResponseWriter &operator=(const HttpResponseWriter &) = delete;

    HttpHeader &Header() { return header_; }
    // Sends the status with the current header on the first write or flush. Later calls
    // are ignored.
    void WriteHeader(int code);
    int Write(const void *data, size_t size);
    int Write(const std::string &s) { return Write(s.data(), s.size()); }
    // Sends what is buffered now, e.g. for server-sent events. Without a declared
    // Content-Length the response becomes chunked.
    int Flush();

    // Takes over the connection, like Go's Hijacker: buffered output is flushed, and the
    // server stops using the connection. br holds bytes the peer sent after the request.
    // Both stay owned by the server and are closed once the handler returns.
    int Hijack(StreamConn **conn, BufReader **br);

    int status() const { return status_; }
    int64_t written() const { return written_; }

 private:
    friend class HttpServerConn;

    // Starts a response to r.
    void Reset(HttpRequest *r);
    // Completes the response after the handler returned.
    int Finish();
    bool ShouldClose() const { return close_; }
    bool Hijacked() const { return hijacked_; }

    // Serializes the header, and the body buffered so far, into out_. final is true when
    // the handler is done, so the body's length is known.
    void Commit(bool final);
    int WriteBody(const char *data, size_t size);
    int WriteIovs(iovec *iov, int n);
    int FlushOut();

    StreamConn *conn_;
    BufReader *br_;
    HttpRequest *req_ = nullptr;
    HttpHeader header_;

    int status_ = 200;
    bool wrote_header_ = false;
    bool committed_ = false;
    bool chunked_ = false;
    bool body_allowed_ = true;
    bool is_head_ = false;
    bool close_ = false;
    bool hijacked_ = false;
    // declared Content-Length, or -1.
    int64_t content_length_ = -1;
    int64_t written_ = 0;
    int err_ = 0;

    // Body bytes written before the header is committed.
    std::string pending_;
    // Committed bytes not sent yet: the header, then framed body.
    std::string out_;
};
