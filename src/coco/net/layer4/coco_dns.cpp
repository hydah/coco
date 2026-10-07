#include "coco/net/layer4/coco_dns.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <sstream>

#include "st.h"

#include "coco/base/task_group.hpp"
#include "coco/common/error.hpp"
#include "coco/net/coco_socket.hpp"
#include "coco/net/layer4/dns_message.hpp"

namespace coco {

namespace {

const char *kResolvConfPath = "/etc/resolv.conf";
const char *kHostsPath = "/etc/hosts";
const int64_t kRecheckUs = 5 * 1000 * 1000;
const uint32_t kMaxTtl = 3600;
const size_t kMaxCacheEntries = 4096;
const int kDnsPort = 53;
const int kMaxNdots = 15;
const int kMaxTimeoutSeconds = 30;
const int kMaxAttempts = 5;
// Servers may ignore the 512-byte UDP limit; a larger datagram must not be cut off.
const size_t kRecvBuffer = 4096;

int64_t NowUs() { return (int64_t)st_utime(); }

std::string ToLower(std::string s) {
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
    }
    return s;
}

std::string TrimDot(const std::string &s) {
    return (!s.empty() && s.back() == '.') ? s.substr(0, s.size() - 1) : s;
}

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

bool ParseServer(const std::string &text, IpAddress *ip, int *port) {
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

bool ReadFile(const char *path, std::string *out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

std::string FileStamp(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return "";
    }
#if defined(__APPLE__)
    long nanoseconds = st.st_mtimespec.tv_nsec;
#else
    long nanoseconds = st.st_mtim.tv_nsec;
#endif
    return std::to_string((long long)st.st_mtime) + ":" + std::to_string(nanoseconds) + ":" +
           std::to_string((long long)st.st_size) +
           ":" + std::to_string((unsigned long long)st.st_ino);
}

bool ValidName(const std::string &name) {
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

// The names to ask for, in order, as resolv.conf(5) describes: a name with a trailing dot
// is tried alone; one with at least ndots dots before the search suffixes, others after.
std::vector<std::string> NameList(const std::string &host, const DnsConfig &config) {
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

uint16_t RandomId() {
    // Off-path spoofing has to guess the ID and the source port, so both are random.
    static thread_local std::random_device rd;
    return (uint16_t)rd();
}

struct Answer {
    // COCO_SUCCESS (addrs may be empty: the name has no record of this type),
    // ERROR_DNS_NOT_FOUND, or the last failure while not done.
    int ret = ERROR_DNS_TIMEOUT;
    bool done = false;
    std::vector<IpAddress> addrs;
    int64_t expire_us = 0;
};

// Reads the addresses for the question, following CNAMEs from the question name. Records
// owned by any other name are ignored, so a server cannot slip in unrelated addresses.
void TakeAddresses(const DnsMessage &msg, uint16_t qtype, Answer *a) {
    a->addrs.clear();
    std::map<std::string, const DnsRecord *> aliases;
    for (const DnsRecord &rr : msg.answers) {
        if (rr.type == kDnsTypeCname && rr.klass == kDnsClassIn) {
            aliases.emplace(ToLower(rr.name), &rr);
        }
    }
    std::string current = ToLower(msg.qname);
    std::set<std::string> names{current};
    uint32_t ttl = kMaxTtl;
    for (;;) {
        auto alias = aliases.find(current);
        if (alias == aliases.end()) {
            break;
        }
        current = ToLower(alias->second->target);
        if (!names.insert(current).second) {
            a->ret = ERROR_DNS_SERVER;
            return;
        }
        ttl = std::min(ttl, alias->second->ttl);
    }

    size_t size = qtype == kDnsTypeA ? 4 : 16;
    for (const DnsRecord &rr : msg.answers) {
        if (rr.type != qtype || rr.klass != kDnsClassIn || rr.data.size() != size) {
            continue;
        }
        if (names.count(ToLower(rr.name)) == 0) {
            continue;
        }
        IpAddress ip;
        ip.family = qtype == kDnsTypeA ? AF_INET : AF_INET6;
        memcpy(ip.bytes, rr.data.data(), size);
        a->addrs.push_back(ip);
        ttl = std::min(ttl, rr.ttl);
    }
    a->expire_us = a->addrs.empty() ? 0 : NowUs() + (int64_t)ttl * 1000 * 1000;
    a->ret = COCO_SUCCESS;
    a->done = true;
}

void ApplyResponse(const DnsMessage &msg, uint16_t qtype, Answer *a) {
    if (msg.rcode == kDnsRcodeNoError) {
        TakeAddresses(msg, qtype, a);
    } else if (msg.rcode == kDnsRcodeNxDomain) {
        a->addrs.clear();
        a->ret = ERROR_DNS_NOT_FOUND;
        a->done = true;
    } else {
        // SERVFAIL, REFUSED and the rest: the next server may do better.
        a->ret = ERROR_DNS_SERVER;
    }
}

bool MatchesQuestion(const DnsMessage &msg, const std::string &name, uint16_t qtype) {
    return msg.response && msg.qtype == qtype && msg.qclass == kDnsClassIn &&
           DnsNameEqual(msg.qname, name);
}

bool SameEndpoint(const sockaddr_storage &a, const sockaddr_storage &b) {
    if (a.ss_family != b.ss_family) {
        return false;
    }
    if (a.ss_family == AF_INET) {
        const sockaddr_in *x = (const sockaddr_in *)&a;
        const sockaddr_in *y = (const sockaddr_in *)&b;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    const sockaddr_in6 *x = (const sockaddr_in6 *)&a;
    const sockaddr_in6 *y = (const sockaddr_in6 *)&b;
    return x->sin6_port == y->sin6_port && x->sin6_scope_id == y->sin6_scope_id &&
           memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr)) == 0;
}

// Maps a failed socket call to a DNS error. Must run right after the call, while errno
// still holds its reason.
int SocketError(int ret) {
    if (errno == EINTR) {
        return ERROR_THREAD_INTERRUPED;
    }
    return ret == ERROR_SOCKET_TIMEOUT ? ERROR_DNS_TIMEOUT : ERROR_DNS_SERVER;
}

int ReadUntil(CocoSocket &skt, void *buf, size_t size, int64_t deadline) {
    size_t received = 0;
    while (received < size) {
        int64_t left = deadline - NowUs();
        if (left <= 0) {
            return ERROR_DNS_TIMEOUT;
        }
        skt.set_recv_timeout(left);
        ssize_t nread = 0;
        int ret = skt.Read((uint8_t *)buf + received, size - received, &nread);
        if (ret != COCO_SUCCESS) {
            return SocketError(ret);
        }
        received += (size_t)nread;
    }
    return COCO_SUCCESS;
}

// Sends query over TCP, with the 2-byte length prefix of RFC 1035 4.2.2, and reads the
// reply into *msg.
int TcpExchange(const IpAddress &server, int port, const std::string &query, int64_t deadline,
                DnsMessage *msg) {
    st_netfd_t stfd = nullptr;
    if (OpenSocket(server.family, SOCK_STREAM, &stfd) != COCO_SUCCESS) {
        return ERROR_DNS_SERVER;
    }
    CocoSocket skt(stfd);
    auto left = [deadline]() { return std::max<int64_t>(deadline - NowUs(), 0); };

    sockaddr_storage sa;
    socklen_t sa_len = server.ToSockaddr(port, &sa);
    if (st_connect(stfd, (sockaddr *)&sa, sa_len, (st_utime_t)left()) != 0) {
        if (errno == EINTR) {
            return ERROR_THREAD_INTERRUPED;
        }
        return errno == ETIME ? ERROR_DNS_TIMEOUT : ERROR_DNS_SERVER;
    }

    std::string out;
    out.push_back((char)(query.size() >> 8));
    out.push_back((char)(query.size() & 0xff));
    out += query;
    skt.set_send_timeout(left());
    int ret = skt.Write((void *)out.data(), out.size(), nullptr);
    if (ret != COCO_SUCCESS) {
        return SocketError(ret);
    }

    uint8_t len[2];
    if ((ret = ReadUntil(skt, len, sizeof(len), deadline)) != COCO_SUCCESS) {
        return ret;
    }
    std::vector<uint8_t> body(((size_t)len[0] << 8) | len[1]);
    if ((ret = ReadUntil(skt, body.data(), body.size(), deadline)) != COCO_SUCCESS) {
        return ret;
    }
    if (DecodeDnsMessage(body.data(), body.size(), msg) != COCO_SUCCESS) {
        return ERROR_DNS_SERVER;
    }
    return COCO_SUCCESS;
}

// Asks one server for every answer not yet done, all queries on one socket at once, and
// waits up to timeout_us for the replies. A failure of the server is recorded in the
// answers; only an interrupt is returned.
int QueryServer(const IpAddress &server, int port, const std::string &name,
                const std::vector<uint16_t> &qtypes, int64_t timeout_us,
                std::vector<Answer> *answers) {
    st_netfd_t stfd = nullptr;
    if (OpenSocket(server.family, SOCK_DGRAM, &stfd) != COCO_SUCCESS) {
        for (Answer &a : *answers) {
            a.ret = a.done ? a.ret : ERROR_DNS_SERVER;
        }
        return COCO_SUCCESS;
    }
    CocoSocket skt(stfd);
    sockaddr_storage to;
    socklen_t to_len = server.ToSockaddr(port, &to);
    skt.set_send_timeout(timeout_us);

    struct Outstanding {
        size_t index;
        uint16_t id;
        std::string query;
    };
    std::vector<Outstanding> outstanding;
    for (size_t i = 0; i < qtypes.size(); ++i) {
        Answer &a = (*answers)[i];
        if (a.done) {
            continue;
        }
        Outstanding o;
        o.index = i;
        o.id = RandomId();
        if (EncodeDnsQuery(o.id, name, qtypes[i], &o.query) != COCO_SUCCESS) {
            a.ret = ERROR_DNS_BAD_NAME;
            a.done = true;
            continue;
        }
        int ret = skt.sendto((void *)o.query.data(), (int)o.query.size(), nullptr,
                             (sockaddr *)&to, (int)to_len);
        if (ret != COCO_SUCCESS) {
            if ((ret = SocketError(ret)) == ERROR_THREAD_INTERRUPED) {
                return ret;
            }
            a.ret = ERROR_DNS_SERVER;
            continue;
        }
        outstanding.push_back(o);
    }

    TaskGroup tcp_queries;
    std::vector<uint8_t> buf(kRecvBuffer);
    int64_t deadline = NowUs() + timeout_us;
    while (!outstanding.empty()) {
        int64_t left = deadline - NowUs();
        if (left <= 0) {
            break;
        }
        skt.set_recv_timeout(left);
        sockaddr_storage from;
        int from_len = sizeof(from);
        ssize_t n = 0;
        int ret = skt.recvfrom(buf.data(), (int)buf.size(), &n, (sockaddr *)&from, &from_len);
        if (ret != COCO_SUCCESS) {
            ret = SocketError(ret);
            if (ret == ERROR_THREAD_INTERRUPED) {
                return ret;
            }
            if (ret == ERROR_DNS_SERVER) {
                for (const Outstanding &o : outstanding) {
                    (*answers)[o.index].ret = ERROR_DNS_SERVER;
                }
                outstanding.clear();
            }
            break;
        }

        DnsMessage msg;
        if (!SameEndpoint(from, to) ||
            DecodeDnsMessage(buf.data(), (size_t)n, &msg) != COCO_SUCCESS) {
            continue;
        }
        auto it = std::find_if(outstanding.begin(), outstanding.end(),
                               [&msg, &name, &qtypes](const Outstanding &o) {
                                   return o.id == msg.id &&
                                          MatchesQuestion(msg, name, qtypes[o.index]);
                               });
        if (it == outstanding.end()) {
            continue;
        }
        Outstanding o = *it;
        outstanding.erase(it);
        Answer &a = (*answers)[o.index];

        if (msg.truncated) {
            uint16_t qtype = qtypes[o.index];
            ret = tcp_queries.Spawn([server, port, o, deadline, name, qtype, answers]() {
                DnsMessage full;
                int result = TcpExchange(server, port, o.query, deadline, &full);
                if (result == ERROR_THREAD_INTERRUPED) {
                    return result;
                }
                Answer &answer = (*answers)[o.index];
                if (result != COCO_SUCCESS || full.id != o.id || full.truncated ||
                    !MatchesQuestion(full, name, qtype)) {
                    answer.ret = result == COCO_SUCCESS ? ERROR_DNS_SERVER : result;
                    return COCO_SUCCESS;
                }
                ApplyResponse(full, qtype, &answer);
                return COCO_SUCCESS;
            });
            if (ret != COCO_SUCCESS) {
                a.ret = ERROR_DNS_SERVER;
            }
            continue;
        }
        ApplyResponse(msg, qtypes[o.index], &a);
    }

    for (const Outstanding &o : outstanding) {
        (*answers)[o.index].ret = ERROR_DNS_TIMEOUT;
    }
    int ret = tcp_queries.Wait();
    return tcp_queries.Cancelled() ? ERROR_THREAD_INTERRUPED : ret;
}

// Goes through the servers attempts times until every answer is done.
int Exchange(const DnsConfig &config, const std::string &name,
             const std::vector<uint16_t> &qtypes, std::vector<Answer> *answers) {
    for (int attempt = 0; attempt < config.attempts; ++attempt) {
        for (const std::string &s : config.servers) {
            bool all_done = std::all_of(answers->begin(), answers->end(),
                                        [](const Answer &a) { return a.done; });
            if (all_done) {
                return COCO_SUCCESS;
            }
            IpAddress server;
            int port = 0;
            if (!ParseServer(s, &server, &port)) {
                continue;
            }
            int ret = QueryServer(server, port, name, qtypes, config.timeout_us, answers);
            if (ret != COCO_SUCCESS) {
                return ret;
            }
        }
    }
    return COCO_SUCCESS;
}

// RFC 6761 6.3: localhost and the names under it are the loopback, never asked of a
// server, where a search suffix could turn them into someone else's name.
bool IsLocalhost(const std::string &lower) {
    static const std::string kLocalhost = "localhost";
    static const std::string kSuffix = ".localhost";
    return lower == kLocalhost ||
           (lower.size() > kSuffix.size() &&
            lower.compare(lower.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0);
}

void SortIpv4First(std::vector<IpAddress> *addrs) {
    std::stable_partition(addrs->begin(), addrs->end(),
                          [](const IpAddress &a) { return a.family == AF_INET; });
}

std::shared_ptr<const DnsConfig> WithDefaults(DnsConfig config) {
    if (config.servers.empty()) {
        config.servers = {"127.0.0.1", "::1"};
    }
    config.attempts = std::max(config.attempts, 1);
    return std::make_shared<const DnsConfig>(config);
}

}  // namespace

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
            config->search = {TrimDot(f[1])};
        } else if (f[0] == "search") {
            config->search.clear();
            for (size_t i = 1; i < f.size(); ++i) {
                config->search.push_back(TrimDot(f[i]));
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
            std::vector<IpAddress> &addrs = (*hosts)[ToLower(TrimDot(f[i]))];
            if (std::find(addrs.begin(), addrs.end(), ip) == addrs.end()) {
                addrs.push_back(ip);
            }
        }
    }
}

Resolver::Resolver()
    : system_(true), config_(WithDefaults(DnsConfig())), hosts_(std::make_shared<DnsHosts>()) {}

Resolver::Resolver(const DnsConfig &config)
    : system_(false), config_(WithDefaults(config)), hosts_(std::make_shared<DnsHosts>()) {}

Resolver::Resolver(const DnsConfig &config, const DnsHosts &hosts)
    : system_(false),
      config_(WithDefaults(config)),
      hosts_(std::make_shared<const DnsHosts>(hosts)) {}

void Resolver::ClearCache() {
    owner_.Check();
    cache_.clear();
}

void Resolver::Refresh() {
    int64_t now = NowUs();
    if (!system_ || now < next_check_us_) {
        return;
    }
    next_check_us_ = now + kRecheckUs;

    std::string stamp = FileStamp(kResolvConfPath);
    if (stamp != resolv_stamp_) {
        resolv_stamp_ = stamp;
        std::string text;
        ReadFile(kResolvConfPath, &text);
        DnsConfig config;
        ParseResolvConf(text, &config);
        config_ = WithDefaults(config);
        cache_.clear();
    }

    stamp = FileStamp(kHostsPath);
    if (stamp != hosts_stamp_) {
        hosts_stamp_ = stamp;
        std::string text;
        ReadFile(kHostsPath, &text);
        auto hosts = std::make_shared<DnsHosts>();
        ParseHostsFile(text, hosts.get());
        hosts_ = hosts;
    }
}

int Resolver::LookupIP(const std::string &host, int family, std::vector<IpAddress> *addrs) {
    owner_.Check();
    addrs->clear();
    if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6) {
        return ERROR_DNS_BAD_NAME;
    }

    std::string name = host;
    if (name.size() >= 2 && name.front() == '[' && name.back() == ']') {
        name = name.substr(1, name.size() - 2);
    }
    IpAddress literal;
    if (IpAddress::Parse(name, &literal)) {
        if (family != AF_UNSPEC && family != literal.family) {
            return ERROR_DNS_NOT_FOUND;
        }
        addrs->push_back(literal);
        return COCO_SUCCESS;
    }

    if (!ValidName(TrimDot(name))) {
        return ERROR_DNS_BAD_NAME;
    }

    Refresh();
    // A lookup may wait for a server while another coroutine reloads the files; it keeps
    // the versions it started with.
    std::shared_ptr<const DnsConfig> config = config_;
    std::shared_ptr<const DnsHosts> hosts = hosts_;

    std::string key = ToLower(TrimDot(name));
    auto found = hosts->find(key);
    if (found != hosts->end()) {
        for (const IpAddress &ip : found->second) {
            if (family == AF_UNSPEC || family == ip.family) {
                addrs->push_back(ip);
            }
        }
        if (!addrs->empty()) {
            SortIpv4First(addrs);
            return COCO_SUCCESS;
        }
    }
    if (IsLocalhost(key)) {
        IpAddress ip;
        if (family != AF_INET6 && IpAddress::Parse("127.0.0.1", &ip)) {
            addrs->push_back(ip);
        }
        if (family != AF_INET && IpAddress::Parse("::1", &ip)) {
            addrs->push_back(ip);
        }
        return COCO_SUCCESS;
    }

    std::vector<uint16_t> qtypes;
    if (family != AF_INET6) {
        qtypes.push_back(kDnsTypeA);
    }
    if (family != AF_INET) {
        qtypes.push_back(kDnsTypeAaaa);
    }

    int last = ERROR_DNS_NOT_FOUND;
    for (const std::string &candidate : NameList(name, *config)) {
        std::vector<Answer> answers(qtypes.size());
        std::vector<bool> cached(qtypes.size(), false);
        int64_t now = NowUs();
        for (size_t i = 0; i < qtypes.size(); ++i) {
            if (config != config_) {
                continue;
            }
            auto it = cache_.find(ToLower(candidate) + "/" + std::to_string(qtypes[i]));
            if (it != cache_.end() && it->second.expire_us > now) {
                answers[i].addrs = it->second.addrs;
                answers[i].ret = COCO_SUCCESS;
                answers[i].done = true;
                cached[i] = true;
            }
        }

        int ret = Exchange(*config, candidate, qtypes, &answers);
        if (ret != COCO_SUCCESS) {
            return ret;
        }

        now = NowUs();
        for (size_t i = 0; i < qtypes.size(); ++i) {
            const Answer &a = answers[i];
            addrs->insert(addrs->end(), a.addrs.begin(), a.addrs.end());
            if (a.ret == ERROR_DNS_SERVER || a.ret == ERROR_DNS_TIMEOUT) {
                last = a.ret;
            }
            if (cached[i] || a.ret != COCO_SUCCESS || a.addrs.empty() || a.expire_us <= now ||
                config != config_) {
                continue;
            }
            if (cache_.size() >= kMaxCacheEntries) {
                for (auto it = cache_.begin(); it != cache_.end();) {
                    it = it->second.expire_us <= now ? cache_.erase(it) : std::next(it);
                }
                if (cache_.size() >= kMaxCacheEntries) {
                    cache_.clear();
                }
            }
            CacheEntry &e = cache_[ToLower(candidate) + "/" + std::to_string(qtypes[i])];
            e.addrs = a.addrs;
            e.expire_us = a.expire_us;
        }
        if (!addrs->empty()) {
            return COCO_SUCCESS;
        }
    }
    return last;
}

Resolver &DefaultResolver() {
    // One per thread, like the runtime its sockets belong to. Never destroyed: coroutines
    // may still resolve during exit.
    static thread_local Resolver *resolver = nullptr;
    if (resolver == nullptr) {
        resolver = new Resolver();
    }
    return *resolver;
}

int LookupHost(const std::string &host, std::vector<std::string> *addrs) {
    std::vector<IpAddress> ips;
    int ret = DefaultResolver().LookupIP(host, AF_UNSPEC, &ips);
    addrs->clear();
    for (const IpAddress &ip : ips) {
        addrs->push_back(ip.ToString());
    }
    return ret;
}

}  // namespace coco
