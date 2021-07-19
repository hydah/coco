#pragma once
#include <map>
#include <sstream>

#include "thirdparty/http-parser/http_parser.h"

#include "protocol/http/http_basic.h"
#include "utils/utils.hpp"

class HttpMuxEntry;
class HttpResponseWriter;
class HttpMessage;
// Objects implementing the Handler interface can be
// registered to serve a particular path or subtree
// in the HTTP server.
//
// ServeHTTP should write reply headers and data to the ResponseWriter
// and then return.  Returning signals that the request is finished
// and that the HTTP server can move on to the next request on
// the connection.
class IHttpHandler {
 public:
    HttpMuxEntry *entry;

 public:
    IHttpHandler();
    virtual ~IHttpHandler();

 public:
    virtual bool is_not_found();
    virtual int serve_http(HttpResponseWriter *w, HttpMessage *r) = 0;
};

// Redirect to a fixed URL
class HttpRedirectHandler : public IHttpHandler {
 private:
    std::string url;
    int code;

 public:
    HttpRedirectHandler(std::string u, int c);
    virtual ~HttpRedirectHandler();

 public:
    virtual int serve_http(HttpResponseWriter *w, HttpMessage *r);
};

// NotFound replies to the request with an HTTP 404 not found error.
class HttpNotFoundHandler : public IHttpHandler {
 public:
    HttpNotFoundHandler();
    virtual ~HttpNotFoundHandler();

 public:
    virtual bool is_not_found();
    virtual int serve_http(HttpResponseWriter *w, HttpMessage *r);
};

// the mux entry for server mux.
// the matcher info, for example, the pattern and handler.
class HttpMuxEntry {
 public:
    bool explicit_match;
    IHttpHandler *handler;
    std::string pattern;
    bool enabled;

 public:
    HttpMuxEntry();
    virtual ~HttpMuxEntry();
};

// ServeMux is an HTTP request multiplexer.
// It matches the URL of each incoming request against a list of registered
// patterns and calls the handler for the pattern that
// most closely matches the URL.
//
// Patterns name fixed, rooted paths, like "/favicon.ico",
// or rooted subtrees, like "/images/" (note the trailing slash).
// Longer patterns take precedence over shorter ones, so that
// if there are handlers registered for both "/images/"
// and "/images/thumbnails/", the latter handler will be
// called for paths beginning "/images/thumbnails/" and the
// former will receive requests for any other paths in the
// "/images/" subtree.
//
// Note that since a pattern ending in a slash names a rooted subtree,
// the pattern "/" matches all paths not matched by other registered
// patterns, not just the URL with Path == "/".
//
// Patterns may optionally begin with a host name, restricting matches to
// URLs on that host only.  Host-specific patterns take precedence over
// general patterns, so that a handler might register for the two patterns
// "/codesearch" and "codesearch.google.com/" without also taking over
// requests for "http://www.google.com/".
//
// ServeMux also takes care of sanitizing the URL request path,
// redirecting any request containing . or .. elements to an
// equivalent .- and ..-free URL.
class HttpServeMux {
 private:
    // the pattern handler, to handle the http request.
    std::map<std::string, HttpMuxEntry *> entries;
    // the vhost handler.
    // when find the handler to process the request,
    // append the matched vhost when pattern not starts with /,
    // for example, for pattern /live/livestream.flv of vhost ossrs.net,
    // the path will rewrite to ossrs.net/live/livestream.flv
    std::map<std::string, IHttpHandler *> vhosts;
    void *connection_;

 public:
    HttpServeMux();
    virtual ~HttpServeMux();

 public:
    /**
     * initialize the http serve mux.
     */
    virtual int initialize();
    void SetConnection(void *connection) { this->connection_ = connection; };

 public:
    // Handle registers the handler for the given pattern.
    // If a handler already exists for pattern, Handle panics.
    virtual int handle(std::string pattern, IHttpHandler *handler);
    virtual void remove_entry(std::string pattern);
    // whether the http muxer can serve the specified message,
    // if not, user can try next muxer.
    virtual bool can_serve(HttpMessage *r);

 public:
    virtual int serve_http(HttpResponseWriter *w, HttpMessage *r);

 private:
    virtual int find_handler(HttpMessage *r, IHttpHandler **ph);
    virtual int match(HttpMessage *r, IHttpHandler **ph);
    virtual bool path_match(std::string pattern, std::string path);
};