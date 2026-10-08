#pragma once

#include <stddef.h>

#include <string>

namespace coco {

// The Date header value for now (RFC 7231 7.1.1.1), formatted once per second by each
// thread.
const std::string &HttpDate();

// Appends "HTTP/1.1 <code> <reason>\r\n"; the reason is "Unknown" for a code without one.
// code is 100 to 999.
void HttpAppendStatusLine(int code, std::string *out);

// The line that starts a chunk of size bytes in chunked encoding, "<hex size>\r\n".
constexpr size_t kHttpChunkHeaderMax = 24;
// Writes it to buf, which holds kHttpChunkHeaderMax bytes, and returns its length.
size_t HttpFormatChunkHeader(size_t size, char *buf);

}  // namespace coco
