#pragma once

#include <stdint.h>
#include <sys/socket.h>

#include <map>
#include <string>
#include <vector>

namespace coco {

struct IpAddress {
    int family = 0;  // AF_INET or AF_INET6
    uint8_t bytes[16] = {};
    uint32_t scope_id = 0;

    // Parses an IPv4 dotted quad or an IPv6 literal with an optional %zone.
    static bool Parse(const std::string &text, IpAddress *out);
    std::string ToString() const;
    // Fills *sa with this address and port; returns its length.
    socklen_t ToSockaddr(int port, sockaddr_storage *sa) const;
    bool operator==(const IpAddress &o) const;
};

struct DnsConfig {
    // "ip", "ip:port" or "[ipv6]:port", port 53 when absent; tried in order.
    std::vector<std::string> servers;
    // Suffixes tried for a name with fewer than ndots dots, as in resolv.conf.
    std::vector<std::string> search;
    int ndots = 1;
    // How long one server gets to answer, and how many times the list is gone through.
    int64_t timeout_us = 5 * 1000 * 1000;
    int attempts = 2;
};

// Lower-case name without the trailing dot -> its addresses, in file order.
typedef std::map<std::string, std::vector<IpAddress>> DnsHosts;

// Applies nameserver, domain, search and options ndots / timeout / attempts lines from
// the text of a resolv.conf. With no nameserver line the servers are 127.0.0.1 and ::1.
void ParseResolvConf(const std::string &text, DnsConfig *config);
void ParseHostsFile(const std::string &text, DnsHosts *hosts);

// One entry of DnsConfig::servers; *port is 53 when it has none.
bool ParseDnsServer(const std::string &text, IpAddress *ip, int *port);

// name without one trailing dot.
std::string DnsTrimDot(const std::string &name);
// A host name a server may be asked for, with or without the trailing dot: letters,
// digits, '-' and '_' in non-empty labels of at most 63 bytes, at most 253 bytes in all.
bool DnsValidName(const std::string &host);
// The names to ask for, in order, as resolv.conf(5) describes: a name with a trailing dot
// is tried alone; one with at least ndots dots before the search suffixes, others after.
// host must be a DnsValidName.
std::vector<std::string> DnsNameList(const std::string &host, const DnsConfig &config);

}  // namespace coco
