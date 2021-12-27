#include "protocol/http/http_basic.h"

#include <map>
#include <string>

void HttpHeader::set(std::string key, std::string value) { headers[key] = value; }

std::string HttpHeader::get(std::string key) {
    std::string v;

    if (headers.find(key) != headers.end()) {
        v = headers[key];
    }

    return v;
}

int64_t HttpHeader::content_length() {
    std::string cl = get("Content-Length");

    if (cl.empty()) {
        return -1;
    }

    return (int64_t)::atof(cl.c_str());
}

void HttpHeader::set_content_length(int64_t size) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%ld", size);
    set("Content-Length", buf);
}

int64_t HttpHeader::get_header_length() { return header_length; }

void HttpHeader::set_header_length(int64_t size) { header_length = size; }

std::string HttpHeader::content_type() { return get("Content-Type"); }

void HttpHeader::set_content_type(std::string ct) { set("Content-Type", ct); }

void HttpHeader::write(std::stringstream &ss) {
    std::map<std::string, std::string>::iterator it;
    for (it = headers.begin(); it != headers.end(); ++it) {
        ss << it->first << ": " << it->second << HTTP_CRLF;
    }
}

std::string HttpHeader::Encode() {
    std::stringstream ss;
    std::map<std::string, std::string>::iterator it;
    for (it = headers.begin(); it != headers.end(); ++it) {
        ss << it->first << ": " << it->second << HTTP_CRLF;
    }
    return ss.str();
}