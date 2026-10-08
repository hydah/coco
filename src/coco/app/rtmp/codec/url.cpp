#include "coco/app/rtmp/codec/url.hpp"

#include <stdlib.h>

#include "coco/common/error.hpp"

namespace coco {

namespace {

long ParsePort(const std::string& s) {
    if (s.empty()) {
        return -1;
    }
    char* end = nullptr;
    long port = strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || port <= 0 || port > 65535) {
        return -1;
    }
    return port;
}

}  // namespace

int ParseRtmpUrl(const std::string& url, RtmpUrl* out) {
    const std::string rtmp = "rtmp://";
    const std::string rtmps = "rtmps://";
    RtmpUrl u;
    size_t off = 0;
    if (url.compare(0, rtmp.size(), rtmp) == 0) {
        off = rtmp.size();
    } else if (url.compare(0, rtmps.size(), rtmps) == 0) {
        u.tls = true;
        off = rtmps.size();
    } else {
        return ERROR_RTMP_URL;
    }
    std::string rest = url.substr(off);
    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "" : rest.substr(slash + 1);
    size_t q = path.find('?');
    if (q != std::string::npos) {
        path.erase(q);
    }
    if (hostport.empty()) {
        return ERROR_RTMP_URL;
    }
    if (hostport[0] == '[') {
        size_t rb = hostport.find(']');
        if (rb == std::string::npos) {
            return ERROR_RTMP_URL;
        }
        u.host = hostport.substr(1, rb - 1);
        if (rb + 1 < hostport.size()) {
            if (hostport[rb + 1] != ':') {
                return ERROR_RTMP_URL;
            }
            long port = ParsePort(hostport.substr(rb + 2));
            if (port < 0) {
                return ERROR_RTMP_URL;
            }
            u.port = (int)port;
        }
    } else {
        size_t colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            u.host = hostport.substr(0, colon);
            long port = ParsePort(hostport.substr(colon + 1));
            if (port < 0) {
                return ERROR_RTMP_URL;
            }
            u.port = (int)port;
        } else {
            u.host = hostport;
        }
    }
    if (u.host.empty()) {
        return ERROR_RTMP_URL;
    }
    while (!path.empty() && path[path.size() - 1] == '/') {
        path.erase(path.size() - 1);
    }
    size_t sep = path.find('/');
    if (path.empty()) {
        return ERROR_RTMP_URL;
    }
    if (sep == std::string::npos) {
        u.app = path;
    } else {
        u.app = path.substr(0, sep);
        u.stream = path.substr(sep + 1);
    }
    if (u.app.empty()) {
        return ERROR_RTMP_URL;
    }
    u.tc_url = u.tls ? "rtmps://" : "rtmp://";
    if (hostport[0] == '[') {
        u.tc_url += "[" + u.host + "]";
    } else {
        u.tc_url += u.host;
    }
    if (u.port != (int)kRtmpDefaultPort) {
        u.tc_url += ":" + std::to_string(u.port);
    }
    u.tc_url += "/" + u.app;
    *out = u;
    return COCO_SUCCESS;
}

}  // namespace coco
