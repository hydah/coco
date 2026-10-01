#include "coco/net/layer7/http/http_basic.h"

#include <string.h>
#include <strings.h>

#include "http-parser/http_parser.h"

namespace coco {

const char *HttpStatusText(int code) {
    switch (code) {
#define XX(num, name, string) \
    case num:                 \
        return #string;
        HTTP_STATUS_MAP(XX)
#undef XX
        default:
            return "";
    }
}

bool HttpBodyAllowedForStatus(int code) {
    if (code >= 100 && code <= 199) {
        return false;
    }
    return code != 204 && code != 304;
}

static bool HasPrefixFold(const char *p, size_t n, const char *prefix) {
    size_t len = strlen(prefix);
    return n >= len && strncasecmp(p, prefix, len) == 0;
}

static bool HasPrefix(const char *p, size_t n, const char *prefix, size_t len) {
    return n >= len && memcmp(p, prefix, len) == 0;
}

std::string HttpDetectContentType(const char *data, size_t size) {
    if (data == nullptr || size == 0) {
        return HttpContentTypeText;
    }
    if (size > 512) {
        size = 512;
    }

    size_t ws = 0;
    while (ws < size && (data[ws] == ' ' || data[ws] == '\t' || data[ws] == '\n' ||
                         data[ws] == '\r' || data[ws] == '\f')) {
        ++ws;
    }
    const char *p = data + ws;
    size_t n = size - ws;
    // WHATWG MIME sniffing: an HTML tag must end with a space or '>'.
    static const char *kHtmlTags[] = {"<!DOCTYPE HTML", "<HTML", "<HEAD", "<SCRIPT", "<IFRAME",
                                      "<H1", "<DIV", "<FONT", "<TABLE", "<A", "<STYLE",
                                      "<TITLE", "<B", "<BODY", "<BR", "<P", "<!--"};
    for (const char *tag : kHtmlTags) {
        size_t len = strlen(tag);
        if (HasPrefixFold(p, n, tag) && n > len && (p[len] == ' ' || p[len] == '>')) {
            return HttpContentTypeHtml;
        }
    }
    if (HasPrefix(p, n, "<?xml", 5)) {
        return "text/xml; charset=utf-8";
    }

    struct Magic {
        const char *sig;
        size_t len;
        const char *type;
    };
    static const Magic kMagics[] = {
        {"%PDF-", 5, "application/pdf"},
        {"\x89PNG\r\n\x1a\n", 8, "image/png"},
        {"\xff\xd8\xff", 3, "image/jpeg"},
        {"GIF87a", 6, "image/gif"},
        {"GIF89a", 6, "image/gif"},
        {"\x1f\x8b\x08", 3, "application/x-gzip"},
        {"PK\x03\x04", 4, "application/zip"},
        {"\xef\xbb\xbf", 3, HttpContentTypeText},
    };
    for (const Magic &m : kMagics) {
        if (HasPrefix(data, size, m.sig, m.len)) {
            return m.type;
        }
    }
    if (size >= 14 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBPVP", 6) == 0) {
        return "image/webp";
    }

    for (size_t i = 0; i < size; ++i) {
        unsigned char c = (unsigned char)data[i];
        if (c <= 0x08 || c == 0x0B || (c >= 0x0E && c <= 0x1A) || (c >= 0x1C && c <= 0x1F)) {
            return HttpContentTypeOctetStream;
        }
    }
    return HttpContentTypeText;
}

static bool EqualFold(const std::string &a, const std::string &b) {
    return a.size() == b.size() && strncasecmp(a.data(), b.data(), a.size()) == 0;
}

static const std::string kEmpty;

const std::string &HttpHeader::Get(const std::string &key) const {
    for (const Field &f : fields_) {
        if (EqualFold(f.first, key)) {
            return f.second;
        }
    }
    return kEmpty;
}

bool HttpHeader::Has(const std::string &key) const {
    for (const Field &f : fields_) {
        if (EqualFold(f.first, key)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> HttpHeader::Values(const std::string &key) const {
    std::vector<std::string> v;
    for (const Field &f : fields_) {
        if (EqualFold(f.first, key)) {
            v.push_back(f.second);
        }
    }
    return v;
}

void HttpHeader::Set(const std::string &key, const std::string &value) {
    bool found = false;
    for (size_t i = 0; i < fields_.size();) {
        if (!EqualFold(fields_[i].first, key)) {
            ++i;
        } else if (!found) {
            fields_[i].second = value;
            found = true;
            ++i;
        } else {
            fields_.erase(fields_.begin() + i);
        }
    }
    if (!found) {
        fields_.push_back(Field(key, value));
    }
}

void HttpHeader::Add(const std::string &key, const std::string &value) {
    fields_.push_back(Field(key, value));
}

void HttpHeader::Del(const std::string &key) {
    for (size_t i = 0; i < fields_.size();) {
        if (EqualFold(fields_[i].first, key)) {
            fields_.erase(fields_.begin() + i);
        } else {
            ++i;
        }
    }
}

bool HttpHeader::HasToken(const std::string &key, const char *token) const {
    size_t len = strlen(token);
    for (const Field &f : fields_) {
        if (!EqualFold(f.first, key)) {
            continue;
        }
        const char *p = f.second.data();
        const char *end = p + f.second.size();
        while (p < end) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) {
                ++p;
            }
            const char *s = p;
            while (p < end && *p != ',') {
                ++p;
            }
            const char *e = p;
            while (e > s && (e[-1] == ' ' || e[-1] == '\t')) {
                --e;
            }
            if ((size_t)(e - s) == len && strncasecmp(s, token, len) == 0) {
                return true;
            }
        }
    }
    return false;
}

void HttpHeader::WriteTo(std::string *out) const {
    for (const Field &f : fields_) {
        out->append(f.first);
        out->append(": ", 2);
        size_t start = out->size();
        out->append(f.second);
        if (f.second.find_first_of("\r\n") != std::string::npos) {
            for (size_t i = start; i < out->size(); ++i) {
                if ((*out)[i] == '\r' || (*out)[i] == '\n') {
                    (*out)[i] = ' ';
                }
            }
        }
        out->append(HTTP_CRLF, 2);
    }
}

HttpValues HttpValues::Parse(const std::string &query) {
    HttpValues v;
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) {
            amp = query.size();
        }
        if (amp > pos) {
            std::string pair = query.substr(pos, amp - pos);
            size_t eq = pair.find('=');
            std::string key, value;
            bool ok = HttpQueryUnescape(pair.substr(0, eq), &key);
            if (ok && eq != std::string::npos) {
                ok = HttpQueryUnescape(pair.substr(eq + 1), &value);
            }
            if (ok) {
                v.values_.push_back(std::make_pair(key, value));
            }
        }
        pos = amp + 1;
    }
    return v;
}

const std::string &HttpValues::Get(const std::string &key) const {
    for (auto &kv : values_) {
        if (kv.first == key) {
            return kv.second;
        }
    }
    return kEmpty;
}

bool HttpValues::Has(const std::string &key) const {
    for (auto &kv : values_) {
        if (kv.first == key) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> HttpValues::Values(const std::string &key) const {
    std::vector<std::string> v;
    for (auto &kv : values_) {
        if (kv.first == key) {
            v.push_back(kv.second);
        }
    }
    return v;
}

void HttpValues::Set(const std::string &key, const std::string &value) {
    Del(key);
    Add(key, value);
}

void HttpValues::Add(const std::string &key, const std::string &value) {
    values_.push_back(std::make_pair(key, value));
}

void HttpValues::Del(const std::string &key) {
    for (size_t i = 0; i < values_.size();) {
        if (values_[i].first == key) {
            values_.erase(values_.begin() + i);
        } else {
            ++i;
        }
    }
}

std::string HttpValues::Encode() const {
    std::string s;
    for (auto &kv : values_) {
        if (!s.empty()) {
            s += '&';
        }
        s += HttpQueryEscape(kv.first);
        s += '=';
        s += HttpQueryEscape(kv.second);
    }
    return s;
}

std::string HttpQueryEscape(const std::string &s) {
    static const char *kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

static int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool Unescape(const std::string &s, bool plus_is_space, std::string *out) {
    if (s.find_first_of(plus_is_space ? "%+" : "%") == std::string::npos) {
        *out = s;
        return true;
    }
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%') {
            if (i + 2 >= s.size()) {
                return false;
            }
            int hi = HexValue(s[i + 1]), lo = HexValue(s[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            r += (char)(hi << 4 | lo);
            i += 2;
        } else if (c == '+' && plus_is_space) {
            r += ' ';
        } else {
            r += c;
        }
    }
    out->swap(r);
    return true;
}

bool HttpQueryUnescape(const std::string &s, std::string *out) { return Unescape(s, true, out); }

bool HttpPathUnescape(const std::string &s, std::string *out) { return Unescape(s, false, out); }

std::string HttpCleanPath(const std::string &p) {
    if (p.empty()) {
        return "/";
    }
    std::vector<std::pair<size_t, size_t>> segs;
    size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p[i] == '/') {
            ++i;
        }
        size_t s = i;
        while (i < p.size() && p[i] != '/') {
            ++i;
        }
        size_t len = i - s;
        if (len == 0 || (len == 1 && p[s] == '.')) {
            continue;
        }
        if (len == 2 && p[s] == '.' && p[s + 1] == '.') {
            if (!segs.empty()) {
                segs.pop_back();
            }
            continue;
        }
        segs.push_back(std::make_pair(s, len));
    }

    std::string np;
    np.reserve(p.size() + 1);
    for (auto &seg : segs) {
        np += '/';
        np.append(p, seg.first, seg.second);
    }
    if (np.empty()) {
        return "/";
    }
    if (p.back() == '/') {
        np += '/';
    }
    return np;
}

}  // namespace coco
