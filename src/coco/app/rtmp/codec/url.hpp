#pragma once

#include <stdint.h>

#include <string>

namespace coco {

constexpr uint32_t kRtmpDefaultPort = 1935;

// rtmp://host[:port]/app[/stream] or rtmps://. app is the first path segment, stream is
// the rest. tc_url is rtmp://host[:port]/app, with the port omitted when it is 1935.
struct RtmpUrl {
    bool tls = false;
    std::string host;
    int port = kRtmpDefaultPort;
    std::string app;
    std::string stream;
    std::string tc_url;
};

int ParseRtmpUrl(const std::string& url, RtmpUrl* out);

}  // namespace coco
