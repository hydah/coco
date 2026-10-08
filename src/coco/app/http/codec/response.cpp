#include "coco/app/http/codec/response.hpp"

#include <stdio.h>
#include <time.h>

#include "coco/app/http/codec/basic.hpp"

namespace coco {

const std::string &HttpDate() {
    static thread_local time_t last = 0;
    static thread_local std::string value;
    time_t now = time(nullptr);
    if (now != last) {
        char buf[64];
        struct tm tm;
        gmtime_r(&now, &tm);
        size_t n = strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
        value.assign(buf, n);
        last = now;
    }
    return value;
}

void HttpAppendStatusLine(int code, std::string *out) {
    char line[64];
    const char *text = HttpStatusText(code);
    int n = snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n", code, *text ? text : "Unknown");
    out->append(line, (size_t)n);
}

size_t HttpFormatChunkHeader(size_t size, char *buf) {
    return (size_t)snprintf(buf, kHttpChunkHeaderMax, "%zx\r\n", size);
}

}  // namespace coco
