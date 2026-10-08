#pragma once

#include <stdint.h>
#include <sys/socket.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "coco/base/owner_thread.hpp"
#include "coco/net/dns/codec/config.hpp"

namespace coco {

// A stub resolver: asks recursive name servers over UDP (TCP when the answer is
// truncated) from the calling coroutine, so only that coroutine waits, never the thread.
// The A and AAAA queries go out together. Answers are cached for their TTL, at most an
// hour; failures are not cached.
//
// It belongs to the thread that created it; any coroutine of that thread may use it,
// several at once.
class Resolver {
 public:
    // Follows /etc/resolv.conf and /etc/hosts, re-read when they change (checked at
    // most every 5 seconds).
    Resolver();
    // Uses config and no hosts file.
    explicit Resolver(const DnsConfig &config);
    Resolver(const DnsConfig &config, const DnsHosts &hosts);

    Resolver(const Resolver &) = delete;
    Resolver &operator=(const Resolver &) = delete;

    // Addresses of host, which may be an IP literal (returned as is, also in brackets),
    // a name in the hosts file, localhost or a name under it (the loopback, RFC 6761,
    // unless the hosts file says otherwise), or a name to ask the servers. family is AF_INET,
    // AF_INET6 or AF_UNSPEC, for which IPv4 addresses come before IPv6 ones.
    //
    // Fails with ERROR_DNS_NOT_FOUND, ERROR_DNS_SERVER, ERROR_DNS_TIMEOUT or
    // ERROR_DNS_BAD_NAME, and with ERROR_THREAD_INTERRUPED at once when the coroutine is
    // interrupted (stopped) while waiting for a server.
    int LookupIP(const std::string &host, int family, std::vector<IpAddress> *addrs);
    void ClearCache();

 private:
    struct CacheEntry {
        std::vector<IpAddress> addrs;
        int64_t expire_us;
    };

    void Refresh();

    bool system_;
    std::shared_ptr<const DnsConfig> config_;
    std::shared_ptr<const DnsHosts> hosts_;
    int64_t next_check_us_ = 0;
    // Identify the version of each file read (mtime, size, inode); empty when missing.
    std::string resolv_stamp_;
    std::string hosts_stamp_;
    std::map<std::string, CacheEntry> cache_;
    OwnerThread owner_;
};

// The resolver of the calling thread, which DialTcp, DialUdp and every client built on
// them use. Created on first use, never destroyed.
Resolver &DefaultResolver();

// Like Go's net.LookupHost: the addresses of host as text, from DefaultResolver().
int LookupHost(const std::string &host, std::vector<std::string> *addrs);

}  // namespace coco
