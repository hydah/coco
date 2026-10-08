#pragma once

#include <string>

namespace coco {

// An absolute http:// or https:// URL, split into what a client request needs.
struct HttpUrl {
    bool tls = false;
    std::string host;
    int port = 80;
    // path and query, as sent on the request line.
    std::string request_uri;
    // host, with brackets for IPv6 and the port when it is not the default.
    std::string host_header;
    // "scheme://host:port", what pooled connections are keyed by.
    std::string key;
};

// ERROR_HTTP_PARSE_URI unless url is http://host... or https://host...
int ParseHttpUrl(const std::string &url, HttpUrl *u);

// Resolves a Location header against the URL that answered with it.
std::string HttpResolveLocation(const HttpUrl &base, const std::string &loc);

}  // namespace coco
