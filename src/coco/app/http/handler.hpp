#pragma once
#include <functional>
#include <string>

#include "coco/app/http/codec/message.hpp"
#include "coco/app/http/response_writer.hpp"

namespace coco {

// Serves one request, like Go's http.Handler. ServeHTTP runs on the connection's
// coroutine; when it returns the response is complete and the connection moves on.
class HttpHandler {
 public:
    virtual ~HttpHandler() = default;
    virtual void ServeHTTP(HttpResponseWriter &w, HttpRequest &r) = 0;
};

typedef std::function<void(HttpResponseWriter &w, HttpRequest &r)> HttpHandlerFunc;

// Adapts a function to HttpHandler, like Go's http.HandlerFunc.
class HttpFuncHandler : public HttpHandler {
 public:
    explicit HttpFuncHandler(HttpHandlerFunc f) : f_(std::move(f)) {}
    void ServeHTTP(HttpResponseWriter &w, HttpRequest &r) override { f_(w, r); }

 private:
    HttpHandlerFunc f_;
};

// Replies with error as plain text and code, like Go's http.Error.
void HttpError(HttpResponseWriter &w, const std::string &error, int code);
// Replies 404.
void HttpNotFound(HttpResponseWriter &w, HttpRequest &r);
// Replies with a redirect to url, which may be relative to the request path.
void HttpRedirect(HttpResponseWriter &w, HttpRequest &r, const std::string &url, int code);

}  // namespace coco
