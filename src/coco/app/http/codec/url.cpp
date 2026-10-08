#include "coco/app/http/codec/url.hpp"

#include <strings.h>

#include "http-parser/http_parser.h"

#include "coco/common/error.hpp"
#include "coco/log/log.hpp"

namespace coco {

int ParseHttpUrl(const std::string &url, HttpUrl *u) {
    http_parser_url pu;
    http_parser_url_init(&pu);
    if (http_parser_parse_url(url.data(), url.size(), 0, &pu) != 0) {
        coco_error("http: bad url %s", url.c_str());
        return ERROR_HTTP_PARSE_URI;
    }
    auto field = [&](http_parser_url_fields f) {
        return (pu.field_set & (1 << f)) ? url.substr(pu.field_data[f].off, pu.field_data[f].len)
                                         : std::string();
    };
    std::string scheme = field(UF_SCHEMA);
    u->tls = strcasecmp(scheme.c_str(), "https") == 0;
    u->host = field(UF_HOST);
    if ((!u->tls && strcasecmp(scheme.c_str(), "http") != 0) || u->host.empty()) {
        coco_error("http: url must be http://host or https://host, got %s", url.c_str());
        return ERROR_HTTP_PARSE_URI;
    }
    int def = u->tls ? 443 : 80;
    u->port = pu.port != 0 ? pu.port : def;
    u->request_uri = field(UF_PATH);
    if (u->request_uri.empty()) {
        u->request_uri = "/";
    }
    if (pu.field_set & (1 << UF_QUERY)) {
        u->request_uri += "?" + field(UF_QUERY);
    }
    // RFC 7230 5.4: an IPv6 literal goes in brackets, and a non-default port is included.
    u->host_header = u->host.find(':') != std::string::npos ? "[" + u->host + "]" : u->host;
    if (u->port != def) {
        u->host_header += ":" + std::to_string(u->port);
    }
    u->key = (u->tls ? "https://" : "http://") + u->host + ":" + std::to_string(u->port);
    return COCO_SUCCESS;
}

std::string HttpResolveLocation(const HttpUrl &base, const std::string &loc) {
    if (loc.find("://") != std::string::npos) {
        return loc;
    }
    std::string scheme = base.tls ? "https:" : "http:";
    if (loc.compare(0, 2, "//") == 0) {
        return scheme + loc;
    }
    std::string origin = scheme + "//" + base.host_header;
    if (!loc.empty() && loc[0] == '/') {
        return origin + loc;
    }
    std::string path = base.request_uri.substr(0, base.request_uri.find('?'));
    return origin + path.substr(0, path.rfind('/') + 1) + loc;
}

}  // namespace coco
