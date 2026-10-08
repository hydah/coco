#include "coco/net/dns/codec/config.hpp"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>

#include <algorithm>
#include <sstream>

#include "coco/net/dns/codec/message.hpp"

namespace coco {

namespace {

const int kDnsPort = 53;
const int kMaxNdots = 15;
const int kMaxTimeoutSeconds = 30;
const int kMaxAttempts = 5;

std::vector<std::string> Fields(const std::string &line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string f;
    while (in >> f) {
        out.push_back(f);
    }
    return out;
}

// The line without a comment starting with one of marks.
std::string StripComment(const std::string &line, const char *marks) {
    size_t pos = line.find_first_of(marks);
    return pos == std::string::npos ? line : line.substr(0, pos);
}

bool ParseUint(const std::string &s, uint32_t max, uint32_t *v) {
    if (s.empty() || s.size() > 10) {
        return false;
    }
    uint64_t n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        n = n * 10 + (uint64_t)(c - '0');
    }
    if (n > max) {
        return false;
    }
    *v = (uint32_t)n;
    return true;
}

}  // namespace

std::string DnsTrimDot(const std::string &s) {
    return (!s.empty() && s.back() == '.') ? s.substr(0, s.size() - 1) : s;
}

bool ParseDnsServer(const std::string &text, IpAddress *ip, int *port) {
    std::string host = text;
    std::string port_text;
    if (!text.empty() && text[0] == '[') {
        size_t close = text.find(']');
        if (close == std::string::npos) {
            return false;
        }
        host = text.substr(1, close - 1);
        std::string rest = text.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') {
                return false;
            }
            port_text = rest.substr(1);
        }
    } else if (std::count(text.begin(), text.end(), ':') == 1) {
        size_t colon = text.find(':');
        host = text.substr(0, colon);
        port_text = text.substr(colon + 1);
    }
    *port = kDnsPort;
    if (!port_text.empty()) {
        uint32_t p = 0;
        if (!ParseUint(port_text, 65535, &p) || p == 0) {
            return false;
        }
        *port = (int)p;
    }
    return IpAddress::Parse(host, ip);
}

bool DnsValidName(const std::string &host) {
    std::string name = DnsTrimDot(host);
    if (name.empty() || name.size() > kDnsMaxName) {
        return false;
    }
    size_t label = 0;
    for (char c : name) {
        if (c == '.') {
            if (label == 0) {
                return false;
            }
            label = 0;
            continue;
        }
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_';
        if (!ok || ++label > 63) {
            return false;
        }
    }
    return label > 0;
}

std::vector<std::string> DnsNameList(const std::string &host, const DnsConfig &config) {
    if (host.back() == '.') {
        return {host.substr(0, host.size() - 1)};
    }
    std::vector<std::string> names;
    int dots = (int)std::count(host.begin(), host.end(), '.');
    if (dots >= config.ndots) {
        names.push_back(host);
    }
    for (const std::string &suffix : config.search) {
        if (host.size() + 1 + suffix.size() <= kDnsMaxName) {
            names.push_back(host + "." + suffix);
        }
    }
    if (dots < config.ndots) {
        names.push_back(host);
    }
    return names;
}

bool IpAddress::Parse(const std::string &text, IpAddress *out) {
    if (text.find('\0') != std::string::npos) {
        return false;
    }
    IpAddress ip;
    if (text.find(':') == std::string::npos) {
        in_addr a;
        if (inet_pton(AF_INET, text.c_str(), &a) != 1) {
            return false;
        }
        ip.family = AF_INET;
        memcpy(ip.bytes, &a, 4);
        *out = ip;
        return true;
    }

    std::string addr = text;
    size_t pct = text.find('%');
    if (pct != std::string::npos) {
        std::string zone = text.substr(pct + 1);
        addr = text.substr(0, pct);
        if (!ParseUint(zone, UINT32_MAX, &ip.scope_id)) {
            ip.scope_id = zone.empty() ? 0 : if_nametoindex(zone.c_str());
        }
        if (ip.scope_id == 0) {
            return false;
        }
    }
    in6_addr a6;
    if (inet_pton(AF_INET6, addr.c_str(), &a6) != 1) {
        return false;
    }
    ip.family = AF_INET6;
    memcpy(ip.bytes, &a6, 16);
    *out = ip;
    return true;
}

std::string IpAddress::ToString() const {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (family == AF_INET) {
        inet_ntop(AF_INET, bytes, buf, sizeof(buf));
        return buf;
    }
    if (family != AF_INET6) {
        return "";
    }
    inet_ntop(AF_INET6, bytes, buf, sizeof(buf));
    std::string s = buf;
    if (scope_id != 0) {
        char name[IF_NAMESIZE] = {0};
        s += "%";
        s += if_indextoname(scope_id, name) ? std::string(name) : std::to_string(scope_id);
    }
    return s;
}

socklen_t IpAddress::ToSockaddr(int port, sockaddr_storage *sa) const {
    memset(sa, 0, sizeof(*sa));
    if (family == AF_INET) {
        sockaddr_in *in = (sockaddr_in *)sa;
        in->sin_family = AF_INET;
        in->sin_port = htons((uint16_t)port);
        memcpy(&in->sin_addr, bytes, 4);
        return sizeof(*in);
    }
    sockaddr_in6 *in6 = (sockaddr_in6 *)sa;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons((uint16_t)port);
    in6->sin6_scope_id = scope_id;
    memcpy(&in6->sin6_addr, bytes, 16);
    return sizeof(*in6);
}

bool IpAddress::operator==(const IpAddress &o) const {
    size_t size = family == AF_INET ? 4 : 16;
    return family == o.family && scope_id == o.scope_id && memcmp(bytes, o.bytes, size) == 0;
}

void ParseResolvConf(const std::string &text, DnsConfig *config) {
    std::vector<std::string> servers;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f = Fields(StripComment(line, "#;"));
        if (f.size() < 2) {
            continue;
        }
        if (f[0] == "nameserver") {
            IpAddress ip;
            if (IpAddress::Parse(f[1], &ip)) {
                servers.push_back(f[1]);
            }
        } else if (f[0] == "domain") {
            config->search = {DnsTrimDot(f[1])};
        } else if (f[0] == "search") {
            config->search.clear();
            for (size_t i = 1; i < f.size(); ++i) {
                config->search.push_back(DnsTrimDot(f[i]));
            }
        } else if (f[0] == "options") {
            for (size_t i = 1; i < f.size(); ++i) {
                uint32_t v = 0;
                const std::string &o = f[i];
                if (o.compare(0, 6, "ndots:") == 0 && ParseUint(o.substr(6), UINT32_MAX, &v)) {
                    config->ndots = (int)std::min<uint32_t>(v, kMaxNdots);
                } else if (o.compare(0, 8, "timeout:") == 0 &&
                           ParseUint(o.substr(8), UINT32_MAX, &v)) {
                    int s = (int)std::min<uint32_t>(std::max<uint32_t>(v, 1), kMaxTimeoutSeconds);
                    config->timeout_us = (int64_t)s * 1000 * 1000;
                } else if (o.compare(0, 9, "attempts:") == 0 &&
                           ParseUint(o.substr(9), UINT32_MAX, &v)) {
                    config->attempts =
                        (int)std::min<uint32_t>(std::max<uint32_t>(v, 1), kMaxAttempts);
                }
            }
        }
    }
    if (!servers.empty()) {
        config->servers = servers;
    }
    if (config->servers.empty()) {
        config->servers = {"127.0.0.1", "::1"};
    }
}

void ParseHostsFile(const std::string &text, DnsHosts *hosts) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f = Fields(StripComment(line, "#"));
        IpAddress ip;
        if (f.size() < 2 || !IpAddress::Parse(f[0], &ip)) {
            continue;
        }
        for (size_t i = 1; i < f.size(); ++i) {
            std::vector<IpAddress> &addrs = (*hosts)[DnsLower(DnsTrimDot(f[i]))];
            if (std::find(addrs.begin(), addrs.end(), ip) == addrs.end()) {
                addrs.push_back(ip);
            }
        }
    }
}

}  // namespace coco
