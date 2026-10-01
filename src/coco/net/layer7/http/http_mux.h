#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "coco/net/layer7/http/http_message.h"
#include "coco/net/layer7/http/http_writer.h"

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

// Routes requests by method, host and path, with Go 1.22 ServeMux patterns:
//
//   "[METHOD ][HOST]/[PATH]"
//
//   "/index.html"           that path only
//   "/static/"              the subtree: "/static/", "/static/a/b"...; "/static" redirects
//   "/"                     everything not matched by another pattern
//   "/users/{id}"           one segment, read with r.PathValue("id")
//   "/files/{path...}"      the rest of the path, possibly empty
//   "/blog/{$}"             "/blog/" only, not its subtree
//   "GET /users/{id}"       GET (and HEAD) only; other methods get 405 with Allow
//   "example.com/"          requests whose Host is example.com
//
// The most specific pattern wins: a literal segment over {name}, over a subtree or
// {name...}, and a method-specific pattern over one without method. Paths with ".", ".."
// or "//" are redirected to their clean form first.
class HttpServeMux : public HttpHandler {
 public:
    HttpServeMux();
    ~HttpServeMux();

    // ERROR_HTTP_PATTERN_EMPTY for an invalid pattern, ERROR_HTTP_PATTERN_DUPLICATED when
    // the same method and path are registered twice.
    int Handle(const std::string &pattern, std::shared_ptr<HttpHandler> handler);
    // Takes ownership of handler.
    int Handle(const std::string &pattern, HttpHandler *handler);
    int HandleFunc(const std::string &pattern, HttpHandlerFunc f);

    void ServeHTTP(HttpResponseWriter &w, HttpRequest &r) override;

 private:
    struct Route;
    struct Node;
    struct Match;

    bool Walk(const Node *n, size_t seg, Match *m) const;
    bool TryRoutes(const std::vector<Route> &routes, size_t rest, bool subtree, Match *m) const;
    bool Find(const std::string &host, const std::string &path, Match *m) const;

    std::unique_ptr<Node> root_;
    std::map<std::string, std::unique_ptr<Node>> hosts_;
};

}  // namespace coco
