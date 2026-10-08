// DNS: the message codec, resolv.conf and hosts parsing, and the resolver against a fake
// name server on the loopback, which runs on the same thread as the lookup.

#include <netinet/in.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "st.h"

#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/net/dns/resolver.hpp"
#include "coco/net/tcp.hpp"
#include "coco/net/udp.hpp"
#include "coco/net/dns/codec/message.hpp"
#include "coco/net/tcp_server.hpp"
#include "test_util.hpp"

using namespace coco;

namespace {

const char *kLoopback = "127.0.0.1";
const int64_t kShortTimeoutUs = 200 * 1000;

std::string Bytes(std::initializer_list<int> v) {
    std::string s;
    for (int b : v) {
        s.push_back((char)b);
    }
    return s;
}

std::string U16(uint16_t v) { return Bytes({v >> 8, v & 0xff}); }
std::string U32(uint32_t v) { return U16((uint16_t)(v >> 16)) + U16((uint16_t)(v & 0xffff)); }

std::string WireName(const std::string &name) {
    std::string out;
    size_t start = 0;
    while (start < name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) {
            dot = name.size();
        }
        out.push_back((char)(dot - start));
        out.append(name, start, dot - start);
        start = dot + 1;
    }
    out.push_back('\0');
    return out;
}

struct Rr {
    std::string name;
    uint16_t type;
    uint32_t ttl;
    std::string data;  // raw RDATA, or the target name of a CNAME
};

Rr A(const std::string &name, int a, int b, int c, int d, uint32_t ttl = 60) {
    return Rr{name, kDnsTypeA, ttl, Bytes({a, b, c, d})};
}

Rr Aaaa(const std::string &name, int last, uint32_t ttl = 60) {
    std::string data(16, '\0');
    data[0] = 0x20;
    data[1] = 0x01;
    data[2] = 0x0d;
    data[3] = (char)0xb8;
    data[15] = (char)last;
    return Rr{name, kDnsTypeAaaa, ttl, data};
}

Rr Cname(const std::string &name, const std::string &target) {
    return Rr{name, kDnsTypeCname, 60, target};
}

// A reply to query (the raw bytes the resolver sent) with the question copied over. An
// answer owned by the question name points back at it, as servers compress.
std::string Reply(const std::string &query, uint8_t rcode, const std::vector<Rr> &answers,
                  bool truncated = false) {
    DnsMessage q;
    DecodeDnsMessage((const uint8_t *)query.data(), query.size(), &q);
    uint16_t flags = (uint16_t)(0x8180 | rcode | (truncated ? 0x0200 : 0));
    std::string out = U16(q.id) + U16(flags) + U16(1) + U16((uint16_t)answers.size()) + U16(0) +
                      U16(0) + query.substr(12);
    for (const Rr &rr : answers) {
        out += DnsNameEqual(rr.name, q.qname) ? Bytes({0xc0, 12}) : WireName(rr.name);
        std::string rdata = rr.type == kDnsTypeCname ? WireName(rr.data) : rr.data;
        out += U16(rr.type) + U16(kDnsClassIn) + U32(rr.ttl) + U16((uint16_t)rdata.size()) + rdata;
    }
    return out;
}

// What the fake server does with a query: the datagrams to send back, possibly none.
typedef std::function<std::vector<std::string>(const DnsMessage &q, const std::string &raw)>
    DnsBehavior;

// A name server on 127.0.0.1:port, served by a coroutine of this thread.
class FakeDns {
 public:
    FakeDns(int port, DnsBehavior behavior) : behavior_(behavior) {
        ok_ = ListenUdp(kLoopback, port, &listener_) == COCO_SUCCESS;
        if (ok_) {
            thread_ = cotest::Go([this]() { Loop(); });
        }
    }
    ~FakeDns() {
        if (thread_) {
            stop_ = true;
            st_thread_interrupt(thread_);
            st_thread_join(thread_, NULL);
        }
    }

    bool ok() const { return ok_; }
    // Question names in the order they arrived.
    std::vector<std::string> names;
    std::vector<uint16_t> types;
    // Delay before each reply.
    int delay_ms = 0;

 private:
    void Loop() {
        char buf[1024];
        while (!stop_) {
            sockaddr_storage from;
            int from_len = sizeof(from);
            ssize_t n = 0;
            if (listener_->RecvFrom(buf, sizeof(buf), &n, (sockaddr *)&from, &from_len) !=
                COCO_SUCCESS) {
                continue;
            }
            std::string raw(buf, n);
            DnsMessage q;
            if (DecodeDnsMessage((const uint8_t *)raw.data(), raw.size(), &q) != COCO_SUCCESS) {
                continue;
            }
            names.push_back(q.qname);
            types.push_back(q.qtype);
            if (delay_ms > 0) {
                CocoSleepMs(delay_ms);
            }
            for (const std::string &r : behavior_(q, raw)) {
                listener_->SendTo((void *)r.data(), (int)r.size(), nullptr, (sockaddr *)&from,
                                  from_len);
            }
        }
    }

    DnsBehavior behavior_;
    std::unique_ptr<UdpListener> listener_;
    st_thread_t thread_ = nullptr;
    bool stop_ = false;
    bool ok_ = false;
};

// Answers example.test with 192.0.2.1 / 2001:db8::1 and everything else with NXDOMAIN.
std::vector<std::string> ExampleZone(const DnsMessage &q, const std::string &raw) {
    if (!DnsNameEqual(q.qname, "example.test")) {
        return {Reply(raw, kDnsRcodeNxDomain, {})};
    }
    if (q.qtype == kDnsTypeA) {
        return {Reply(raw, kDnsRcodeNoError, {A("example.test", 192, 0, 2, 1)})};
    }
    return {Reply(raw, kDnsRcodeNoError, {Aaaa("example.test", 1)})};
}

DnsConfig ConfigFor(int port, int64_t timeout_us = kShortTimeoutUs, int attempts = 1) {
    DnsConfig c;
    c.servers = {std::string(kLoopback) + ":" + std::to_string(port)};
    c.timeout_us = timeout_us;
    c.attempts = attempts;
    return c;
}

std::vector<std::string> Texts(const std::vector<IpAddress> &addrs) {
    std::vector<std::string> out;
    for (const IpAddress &a : addrs) {
        out.push_back(a.ToString());
    }
    return out;
}

typedef std::vector<std::string> Strings;

}  // namespace

COTEST(DnsQueryRoundTrip) {
    std::string q;
    CHECK_EQ(EncodeDnsQuery(0x1234, "www.Example.test", kDnsTypeAaaa, &q), COCO_SUCCESS);
    CHECK(q.substr(0, 4) == Bytes({0x12, 0x34, 0x01, 0x00}));

    DnsMessage m;
    CHECK_EQ(DecodeDnsMessage((const uint8_t *)q.data(), q.size(), &m), COCO_SUCCESS);
    CHECK_EQ(m.id, 0x1234);
    CHECK(!m.response);
    CHECK(m.qname == "www.Example.test");
    CHECK_EQ(m.qtype, kDnsTypeAaaa);
    CHECK_EQ(m.qclass, kDnsClassIn);
    CHECK(DnsNameEqual("WWW.example.TEST.", m.qname));
    CHECK(!DnsNameEqual("www.example.tes", m.qname));

    CHECK_EQ(EncodeDnsQuery(1, "", kDnsTypeA, &q), ERROR_DNS_BAD_NAME);
    CHECK_EQ(EncodeDnsQuery(1, "a..b", kDnsTypeA, &q), ERROR_DNS_BAD_NAME);
    CHECK_EQ(EncodeDnsQuery(1, std::string(64, 'a') + ".test", kDnsTypeA, &q), ERROR_DNS_BAD_NAME);
    CHECK_EQ(EncodeDnsQuery(1, std::string(63, 'a') + ".test", kDnsTypeA, &q), COCO_SUCCESS);
}

COTEST(DnsDecodeAnswers) {
    std::string q;
    EncodeDnsQuery(7, "www.example.test", kDnsTypeA, &q);
    std::string r = Reply(q, kDnsRcodeNoError,
                          {Cname("www.example.test", "edge.cdn.test"), A("edge.cdn.test", 10, 0, 0, 1, 30)},
                          true);
    DnsMessage m;
    CHECK_EQ(DecodeDnsMessage((const uint8_t *)r.data(), r.size(), &m), COCO_SUCCESS);
    CHECK(m.response);
    CHECK(m.truncated);
    CHECK_EQ(m.rcode, kDnsRcodeNoError);
    CHECK_EQ(m.answers.size(), 2);
    CHECK(m.answers[0].name == "www.example.test");  // read through the pointer
    CHECK(m.answers[0].target == "edge.cdn.test");
    CHECK(m.answers[1].data == Bytes({10, 0, 0, 1}));
    CHECK_EQ(m.answers[1].ttl, 30);
}

COTEST(DnsDecodeRejectsMalformed) {
    std::string q;
    EncodeDnsQuery(7, "a.test", kDnsTypeA, &q);
    std::string ok = Reply(q, kDnsRcodeNoError, {A("a.test", 1, 2, 3, 4)});
    DnsMessage m;
    CHECK_EQ(DecodeDnsMessage((const uint8_t *)ok.data(), ok.size(), &m), COCO_SUCCESS);

    // Every prefix is cut short somewhere.
    for (size_t n = 0; n < ok.size(); ++n) {
        CHECK_EQ(DecodeDnsMessage((const uint8_t *)ok.data(), n, &m), ERROR_DNS_PROTOCOL);
    }

    // A pointer to itself, and one pointing forwards.
    std::string header = U16(1) + U16(0x8180) + U16(1) + U16(0) + U16(0) + U16(0);
    std::string loop = header + Bytes({0xc0, 12}) + U16(1) + U16(1);
    CHECK_EQ(DecodeDnsMessage((const uint8_t *)loop.data(), loop.size(), &m), ERROR_DNS_PROTOCOL);
    std::string forward = header + Bytes({0xc0, 20}) + U16(1) + U16(1) + Bytes({1, 'a', 0});
    CHECK_EQ(DecodeDnsMessage((const uint8_t *)forward.data(), forward.size(), &m),
             ERROR_DNS_PROTOCOL);

    // A name longer than 255 bytes on the wire.
    std::string longname;
    for (int i = 0; i < 5; ++i) {
        longname += Bytes({63}) + std::string(63, 'a');
    }
    std::string big = header + longname + Bytes({0}) + U16(1) + U16(1);
    CHECK_EQ(DecodeDnsMessage((const uint8_t *)big.data(), big.size(), &m), ERROR_DNS_PROTOCOL);

    for (const std::string &label : Strings({"a.test", std::string("a\0test", 6)})) {
        std::string wire_label = Bytes({(int)label.size()}) + label + Bytes({0});
        std::string bad_question = header + wire_label + U16(kDnsTypeA) + U16(kDnsClassIn);
        CHECK_EQ(DecodeDnsMessage((const uint8_t *)bad_question.data(), bad_question.size(), &m),
                 ERROR_DNS_PROTOCOL);

        std::string bad_owner = ok;
        bad_owner.replace(q.size(), 2, wire_label);
        CHECK_EQ(DecodeDnsMessage((const uint8_t *)bad_owner.data(), bad_owner.size(), &m),
                 ERROR_DNS_PROTOCOL);

        std::string bad_target = Reply(q, kDnsRcodeNoError, {Cname("a.test", "edge.test")});
        bad_target = bad_target.substr(0, q.size() + 10) + U16((uint16_t)wire_label.size()) +
                     wire_label;
        CHECK_EQ(DecodeDnsMessage((const uint8_t *)bad_target.data(), bad_target.size(), &m),
                 ERROR_DNS_PROTOCOL);
    }

    std::string chain_data = WireName("a.test");
    std::string chain_reply = Reply(q, kDnsRcodeNoError, {Rr{"a.test", 65000, 60, chain_data}});
    const size_t rdata_start = chain_reply.size() - chain_data.size();
    size_t chain_tail = rdata_start;
    for (int step = 0; step < 300; ++step) {
        size_t pointer_offset = rdata_start + chain_data.size();
        chain_data += U16((uint16_t)(0xc000 | chain_tail));
        chain_tail = pointer_offset;
    }
    chain_reply = Reply(q, kDnsRcodeNoError,
                        {Rr{"a.test", 65000, 60, chain_data}, A("a.test", 1, 2, 3, 4)});
    const size_t owner_offset = rdata_start + chain_data.size();
    CHECK(chain_tail < owner_offset);
    CHECK(chain_reply.size() < 16384);
    chain_reply.replace(owner_offset, 2, U16((uint16_t)(0xc000 | chain_tail)));
    CHECK_EQ(DecodeDnsMessage((const uint8_t *)chain_reply.data(), chain_reply.size(), &m),
             ERROR_DNS_PROTOCOL);
}

COTEST(DnsParsesConfigFiles) {
    DnsConfig c;
    ParseResolvConf("# comment\n"
                    "nameserver 10.0.0.1\n"
                    "nameserver fe80::1%1 ; zone by index\n"
                    "nameserver not-an-ip\n"
                    "domain old.test\n"
                    "search corp.test. lab.test\n"
                    "options ndots:3 timeout:2 attempts:9 rotate\n",
                    &c);
    CHECK(c.servers == Strings({"10.0.0.1", "fe80::1%1"}));
    CHECK(c.search == Strings({"corp.test", "lab.test"}));
    CHECK_EQ(c.ndots, 3);
    CHECK_EQ(c.timeout_us, 2 * 1000 * 1000);
    CHECK_EQ(c.attempts, 5);

    DnsConfig empty;
    ParseResolvConf("", &empty);
    CHECK(empty.servers == Strings({"127.0.0.1", "::1"}));

    DnsHosts hosts;
    ParseHostsFile("127.0.0.1 localhost Local.Test.  # loopback\n"
                   "::1 localhost\n"
                   "bogus name\n"
                   "10.1.1.1 local.test\n",
                   &hosts);
    CHECK(Texts(hosts["localhost"]) == Strings({"127.0.0.1", "::1"}));
    CHECK(Texts(hosts["local.test"]) == Strings({"127.0.0.1", "10.1.1.1"}));
    CHECK(hosts.count("bogus") == 0);
    CHECK(hosts.count("name") == 0);
}

COTEST(DnsLiteralsAndHosts) {
    DnsHosts hosts;
    ParseHostsFile("::1 db.test\n10.0.0.5 db.test\n", &hosts);
    DnsConfig config = ConfigFor(19351);
    Resolver r(config, hosts);
    std::vector<IpAddress> addrs;

    CHECK_EQ(r.LookupIP("192.0.2.7", AF_UNSPEC, &addrs), COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"192.0.2.7"}));
    CHECK_EQ(r.LookupIP("[2001:db8::7]", AF_UNSPEC, &addrs), COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"2001:db8::7"}));
    CHECK_EQ(r.LookupIP("192.0.2.7", AF_INET6, &addrs), ERROR_DNS_NOT_FOUND);

    // From the hosts file, without a server: IPv4 first, case and trailing dot ignored.
    CHECK_EQ(r.LookupIP("DB.test.", AF_UNSPEC, &addrs), COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"10.0.0.5", "::1"}));
    CHECK_EQ(r.LookupIP("db.test", AF_INET6, &addrs), COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"::1"}));

    // localhost is the loopback even when the hosts file does not say so; nothing listens
    // on the configured server, so asking it would fail.
    CHECK_EQ(r.LookupIP("localhost", AF_UNSPEC, &addrs), COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"127.0.0.1", "::1"}));
    CHECK_EQ(r.LookupIP("API.localhost.", AF_INET, &addrs), COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"127.0.0.1"}));
    CHECK_EQ(r.LookupIP("notlocalhost", AF_INET, &addrs) == COCO_SUCCESS, false);
    DnsHosts own;
    ParseHostsFile("10.0.0.9 localhost\n", &own);
    Resolver with_own(config, own);
    CHECK_EQ(with_own.LookupIP("localhost", AF_UNSPEC, &addrs), COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"10.0.0.9"}));

    CHECK_EQ(r.LookupIP("bad name", AF_UNSPEC, &addrs), ERROR_DNS_BAD_NAME);
    CHECK_EQ(r.LookupIP("a..test", AF_UNSPEC, &addrs), ERROR_DNS_BAD_NAME);
    CHECK_EQ(r.LookupIP("", AF_UNSPEC, &addrs), ERROR_DNS_BAD_NAME);
    CHECK_EQ(r.LookupIP(std::string("192.0.2.7") + '\0' + "ignored", AF_UNSPEC, &addrs),
             ERROR_DNS_BAD_NAME);
    CHECK_EQ(r.LookupIP(std::string("2001:db8::7") + '\0' + "ignored", AF_UNSPEC, &addrs),
             ERROR_DNS_BAD_NAME);
    CHECK_EQ(r.LookupIP("a..localhost", AF_INET, &addrs), ERROR_DNS_BAD_NAME);
}

COTEST(DnsResolverAsksServerAndCaches) {
    const int port = 19352;
    FakeDns server(port, ExampleZone);
    CHECK(server.ok());
    Resolver r(ConfigFor(port));
    std::vector<IpAddress> addrs;

    CHECK_EQ(r.LookupIP("example.test", AF_UNSPEC, &addrs), COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"192.0.2.1", "2001:db8::1"}));
    CHECK_EQ(server.names.size(), 2);  // A and AAAA, sent together

    // Both answers are cached.
    CHECK_EQ(r.LookupIP("EXAMPLE.test", AF_UNSPEC, &addrs), COCO_SUCCESS);
    CHECK_EQ(addrs.size(), 2);
    CHECK_EQ(r.LookupIP("example.test", AF_INET6, &addrs), COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"2001:db8::1"}));
    CHECK_EQ(server.names.size(), 2);

    r.ClearCache();
    CHECK_EQ(r.LookupIP("example.test", AF_INET, &addrs), COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"192.0.2.1"}));
    CHECK_EQ(server.names.size(), 3);
    CHECK_EQ(server.types.back(), kDnsTypeA);

    CHECK_EQ(r.LookupIP("missing.test", AF_UNSPEC, &addrs), ERROR_DNS_NOT_FOUND);
    CHECK(addrs.empty());
}

COTEST(DnsResolverFollowsCnameOnly) {
    const int port = 19353;
    FakeDns server(port, [](const DnsMessage &q, const std::string &raw) {
        if (q.qtype != kDnsTypeA) {
            return std::vector<std::string>{Reply(raw, kDnsRcodeNoError, {})};
        }
        if (DnsNameEqual(q.qname, "self.site.test")) {
            return std::vector<std::string>{
                Reply(raw, kDnsRcodeNoError,
                      {Cname(q.qname, q.qname), A(q.qname, 6, 6, 6, 1)})};
        }
        if (DnsNameEqual(q.qname, "loop.site.test")) {
            return std::vector<std::string>{
                Reply(raw, kDnsRcodeNoError,
                      {Cname(q.qname, "loop.edge.test"), Cname("loop.edge.test", q.qname),
                       A(q.qname, 6, 6, 6, 2), A("loop.edge.test", 6, 6, 6, 3)})};
        }
        // The record for other.test is not on the chain and must be ignored.
        return std::vector<std::string>{
            Reply(raw, kDnsRcodeNoError,
                  {Cname("www.site.test", "edge.cdn.test"), A("other.test", 6, 6, 6, 6),
                   A("edge.cdn.test", 10, 0, 0, 1), A("EDGE.cdn.test", 10, 0, 0, 2)})};
    });
    Resolver r(ConfigFor(port));
    std::vector<IpAddress> addrs;
    CHECK_EQ(r.LookupIP("www.site.test", AF_UNSPEC, &addrs), COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"10.0.0.1", "10.0.0.2"}));

    CHECK_EQ(r.LookupIP("self.site.test", AF_INET, &addrs), ERROR_DNS_SERVER);
    CHECK(addrs.empty());
    CHECK_EQ(r.LookupIP("loop.site.test", AF_INET, &addrs), ERROR_DNS_SERVER);
    CHECK(addrs.empty());
}

COTEST(DnsResolverSearchList) {
    const int port = 19354;
    FakeDns server(port, [](const DnsMessage &q, const std::string &raw) {
        if (DnsNameEqual(q.qname, "db.corp.test") && q.qtype == kDnsTypeA) {
            return std::vector<std::string>{
                Reply(raw, kDnsRcodeNoError, {A("db.corp.test", 10, 9, 9, 9)})};
        }
        return std::vector<std::string>{Reply(raw, kDnsRcodeNxDomain, {})};
    });
    DnsConfig c = ConfigFor(port);
    c.search = {"lab.test", "corp.test"};
    Resolver r(c);
    std::vector<IpAddress> addrs;

    // Fewer dots than ndots: the suffixes first, the bare name last.
    CHECK_EQ(r.LookupIP("db", AF_INET, &addrs), COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"10.9.9.9"}));
    CHECK(server.names == Strings({"db.lab.test", "db.corp.test"}));

    server.names.clear();
    CHECK_EQ(r.LookupIP("x.y", AF_INET, &addrs), ERROR_DNS_NOT_FOUND);
    CHECK(server.names == Strings({"x.y", "x.y.lab.test", "x.y.corp.test"}));

    // A trailing dot means the name is complete.
    server.names.clear();
    CHECK_EQ(r.LookupIP("db.", AF_INET, &addrs), ERROR_DNS_NOT_FOUND);
    CHECK(server.names == Strings({"db"}));
}

COTEST(DnsResolverServerFailures) {
    const int bad = 19355, good = 19356;
    FakeDns failing(bad, [](const DnsMessage &, const std::string &raw) {
        return std::vector<std::string>{Reply(raw, kDnsRcodeServFail, {})};
    });
    FakeDns working(good, ExampleZone);
    std::vector<IpAddress> addrs;

    Resolver only_bad(ConfigFor(bad));
    CHECK_EQ(only_bad.LookupIP("example.test", AF_INET, &addrs), ERROR_DNS_SERVER);

    // SERVFAIL moves on to the next server.
    DnsConfig both = ConfigFor(bad);
    both.servers.push_back(std::string(kLoopback) + ":" + std::to_string(good));
    Resolver r(both);
    CHECK_EQ(r.LookupIP("example.test", AF_INET, &addrs), COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"192.0.2.1"}));
    CHECK_EQ(failing.names.size(), 2);
    CHECK_EQ(working.names.size(), 1);
}

COTEST(DnsResolverRetriesThenTimesOut) {
    const int port = 19357;
    int seen = 0;
    FakeDns server(port, [&seen](const DnsMessage &q, const std::string &raw) {
        // Drops the first query, as a lost datagram.
        if (++seen == 1) {
            return std::vector<std::string>{};
        }
        return ExampleZone(q, raw);
    });
    std::vector<IpAddress> addrs;

    Resolver r(ConfigFor(port, kShortTimeoutUs, 2));
    CHECK_EQ(r.LookupIP("example.test", AF_INET, &addrs), COCO_SUCCESS);
    CHECK_EQ(seen, 2);

    // Nobody listens on this port: every attempt waits out its timeout.
    Resolver silent(ConfigFor(19358, 100 * 1000, 2));
    int64_t start = (int64_t)st_utime();
    CHECK_EQ(silent.LookupIP("example.test", AF_UNSPEC, &addrs), ERROR_DNS_TIMEOUT);
    int64_t took = (int64_t)st_utime() - start;
    CHECK(took >= 180 * 1000);
    CHECK(took < 1000 * 1000);
}

COTEST(DnsResolverIgnoresForgedReplies) {
    const int port = 19359;
    int forged_case = 0;
    FakeDns server(port, [&forged_case](const DnsMessage &q, const std::string &raw) {
        std::string wrong_id = Reply(raw, kDnsRcodeNoError, {A("example.test", 6, 6, 6, 6)});
        wrong_id[0] = (char)(wrong_id[0] ^ 0xff);
        std::string other_q;
        EncodeDnsQuery(q.id, "evil.test", q.qtype, &other_q);
        std::string wrong_question = Reply(other_q, kDnsRcodeNoError, {A("evil.test", 6, 6, 6, 7)});
        std::string wrong_opcode = Reply(raw, kDnsRcodeNoError, {A(q.qname, 6, 6, 6, 8)});
        wrong_opcode[2] = (char)(wrong_opcode[2] | 0x08);
        std::string two_questions = Reply(raw, kDnsRcodeNoError, {A(q.qname, 6, 6, 6, 9)});
        two_questions[5] = 2;
        two_questions.insert(raw.size(), other_q.substr(12));
        std::string wire_label = Bytes({(int)q.qname.size()}) + q.qname + Bytes({0});
        std::string wrong_label_question = Reply(raw, kDnsRcodeNoError, {A(q.qname, 6, 6, 6, 10)});
        wrong_label_question.replace(12, raw.size() - 12,
                                     wire_label + U16(q.qtype) + U16(q.qclass));
        std::string wrong_label_owner = Reply(raw, kDnsRcodeNoError, {A(q.qname, 6, 6, 6, 11)});
        wrong_label_owner.replace(raw.size(), 2, wire_label);
        std::vector<std::string> forged{std::string(), wrong_opcode, two_questions,
                                       wrong_label_question, wrong_label_owner};
        std::vector<std::string> out{forged[forged_case], wrong_id, wrong_question, "garbage"};
        for (const std::string &r : ExampleZone(q, raw)) {
            out.push_back(r);
        }
        return out;
    });
    CHECK(server.ok());
    for (forged_case = 0; forged_case < 5; ++forged_case) {
        Resolver resolver(ConfigFor(port));
        std::vector<IpAddress> addrs;
        size_t queries_before = server.names.size();
        CHECK_EQ(resolver.LookupIP("example.test", AF_INET, &addrs), COCO_SUCCESS);
        CHECK(Texts(addrs) == Strings({"192.0.2.1"}));
        CHECK_EQ(server.names.size(), queries_before + 1);
        CHECK_EQ(resolver.LookupIP("example.test", AF_INET, &addrs), COCO_SUCCESS);
        CHECK(Texts(addrs) == Strings({"192.0.2.1"}));
        CHECK_EQ(server.names.size(), queries_before + 1);
    }
}

COTEST(DnsResolverFallsBackToTcp) {
    const int port = 19360;
    std::vector<Rr> many;
    for (int i = 1; i <= 40; ++i) {
        many.push_back(A("big.test", 10, 0, 1, i));
    }
    FakeDns udp(port, [](const DnsMessage &, const std::string &raw) {
        return std::vector<std::string>{Reply(raw, kDnsRcodeNoError, {}, true)};
    });
    int tcp_queries = 0;
    TcpServer tcp([&](StreamConn &conn) {
        uint8_t len[2];
        if (conn.ReadFully(len, 2, nullptr) != COCO_SUCCESS) {
            return 0;
        }
        std::string query((len[0] << 8) | len[1], '\0');
        if (conn.ReadFully(&query[0], query.size(), nullptr) != COCO_SUCCESS) {
            return 0;
        }
        ++tcp_queries;
        DnsMessage question;
        DecodeDnsMessage((const uint8_t *)query.data(), query.size(), &question);
        std::string reply = DnsNameEqual(question.qname, "truncated.test")
                    ? Reply(query, kDnsRcodeNoError,
                        {A("truncated.test", 6, 6, 6, 6)}, true)
                    : Reply(query, kDnsRcodeNoError, many);
        std::string out = U16((uint16_t)reply.size()) + reply;
        conn.Write((void *)out.data(), out.size(), nullptr);
        return 0;
    });
    CHECK_EQ(tcp.Start(kLoopback, port), COCO_SUCCESS);

    Resolver r(ConfigFor(port, 1000 * 1000));
    std::vector<IpAddress> addrs;
    CHECK_EQ(r.LookupIP("big.test", AF_INET, &addrs), COCO_SUCCESS);
    CHECK_EQ(addrs.size(), 40);
    CHECK_EQ(tcp_queries, 1);

    Resolver truncated(ConfigFor(port, 1000 * 1000));
    for (int attempt = 0; attempt < 2; ++attempt) {
        CHECK_EQ(truncated.LookupIP("truncated.test", AF_INET, &addrs), ERROR_DNS_SERVER);
        CHECK(addrs.empty());
        CHECK_EQ(tcp_queries, attempt + 2);
        CHECK_EQ(udp.names.size(), attempt + 2);
    }
    tcp.Stop();
}

COTEST(DnsResolverTcpUsesTotalDeadline) {
    const int port = 19361;
    FakeDns udp(port, [](const DnsMessage &, const std::string &raw) {
        return std::vector<std::string>{Reply(raw, kDnsRcodeNoError, {}, true)};
    });
    CHECK(udp.ok());
    int tcp_queries = 0;
    TcpServer tcp([&tcp_queries](StreamConn &conn) {
        uint8_t length[2];
        if (conn.ReadFully(length, 2, nullptr) != COCO_SUCCESS) {
            return 0;
        }
        std::string query((length[0] << 8) | length[1], '\0');
        if (conn.ReadFully(&query[0], query.size(), nullptr) != COCO_SUCCESS) {
            return 0;
        }
        ++tcp_queries;
        std::string reply = Reply(query, kDnsRcodeNoError, {A("slow.test", 192, 0, 2, 1)});
        std::string prefix = U16((uint16_t)reply.size());
        if (conn.Write((void *)prefix.data(), prefix.size(), nullptr) != COCO_SUCCESS) {
            return 0;
        }
        for (size_t sent = 0; sent < reply.size(); ++sent) {
            if (st_usleep(35 * 1000) != 0 ||
                conn.Write((void *)(reply.data() + sent), 1, nullptr) != COCO_SUCCESS) {
                break;
            }
        }
        return 0;
    });
    CHECK_EQ(tcp.Start(kLoopback, port), COCO_SUCCESS);

    Resolver resolver(ConfigFor(port, 250 * 1000));
    std::vector<IpAddress> addrs;
    int64_t start = (int64_t)st_utime();
    int result = resolver.LookupIP("slow.test", AF_INET, &addrs);
    int64_t elapsed = (int64_t)st_utime() - start;
    tcp.Stop();
    CHECK_EQ(tcp_queries, 1);
    CHECK_EQ(result, ERROR_DNS_TIMEOUT);
    CHECK(addrs.empty());
    CHECK(elapsed >= 200 * 1000);
    CHECK(elapsed < 500 * 1000);
}

COTEST(DnsResolverKeepsUdpSiblingDuringTcpFallback) {
    const int port = 19362;
    FakeDns udp(port, [](const DnsMessage &q, const std::string &raw) {
        if (q.qtype == kDnsTypeA) {
            return std::vector<std::string>{Reply(raw, kDnsRcodeNoError, {}, true)};
        }
        return ExampleZone(q, raw);
    });
    CHECK(udp.ok());
    int tcp_connections = 0;
    TcpServer tcp([&tcp_connections](StreamConn &) {
        ++tcp_connections;
        while (st_usleep(25 * 1000) == 0) {
        }
        return 0;
    });
    CHECK_EQ(tcp.Start(kLoopback, port), COCO_SUCCESS);

    Resolver resolver(ConfigFor(port, 400 * 1000));
    std::vector<IpAddress> addrs;
    int result = resolver.LookupIP("example.test", AF_UNSPEC, &addrs);
    tcp.Stop();
    CHECK_EQ(tcp_connections, 1);
    CHECK_EQ(udp.names.size(), 2);
    CHECK_EQ(result, COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"2001:db8::1"}));
}

COTEST(DnsResolverExpiresTtlWhileSiblingPending) {
    const int port = 19363;
    int a_queries = 0;
    int64_t first_a_reply_us = 0;
    FakeDns server(port, [&a_queries, &first_a_reply_us](const DnsMessage &q,
                                                     const std::string &raw) {
        if (q.qtype != kDnsTypeA) {
            return std::vector<std::string>{};
        }
        ++a_queries;
        if (first_a_reply_us == 0) {
            first_a_reply_us = (int64_t)st_utime();
        }
        return std::vector<std::string>{Reply(raw, kDnsRcodeNoError, {A(q.qname, 192, 0, 2, 1, 1)})};
    });
    CHECK(server.ok());
    Resolver resolver(ConfigFor(port, 1200 * 1000));
    std::vector<IpAddress> addrs;
    CHECK_EQ(resolver.LookupIP("short-ttl.test", AF_UNSPEC, &addrs), COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"192.0.2.1"}));
    CHECK_EQ(a_queries, 1);
    CHECK_EQ(server.names.size(), 2);
    CHECK(first_a_reply_us > 0);
    CHECK((int64_t)st_utime() - first_a_reply_us >= 1000 * 1000);

    CHECK_EQ(resolver.LookupIP("short-ttl.test", AF_INET, &addrs), COCO_SUCCESS);
    CHECK(Texts(addrs) == Strings({"192.0.2.1"}));
    CHECK_EQ(a_queries, 2);
    CHECK_EQ(server.names.size(), 3);
}

COTEST(DnsLookupDoesNotBlockTheThread) {
    const int port = 19352;
    FakeDns server(port, ExampleZone);
    server.delay_ms = 100;
    int ticks = 0;
    bool done = false;
    st_thread_t ticker = cotest::Go([&]() {
        while (!done) {
            ++ticks;
            CocoSleepMs(5);
        }
    });

    Resolver r(ConfigFor(port, 1000 * 1000));
    std::vector<IpAddress> addrs;
    CHECK_EQ(r.LookupIP("example.test", AF_INET, &addrs), COCO_SUCCESS);
    done = true;
    st_thread_join(ticker, NULL);
    // The server, on this thread too, could only answer because the lookup yielded.
    CHECK(ticks >= 10);
}

COTEST(DnsLookupStopsWhenInterrupted) {
    const int port = 19353;
    FakeDns server(port, [](const DnsMessage &, const std::string &) {
        return std::vector<std::string>{};
    });
    int ret = 0;
    st_thread_t lookup = cotest::Go([&]() {
        Resolver r(ConfigFor(port, 5 * 1000 * 1000, 2));
        std::vector<IpAddress> addrs;
        ret = r.LookupIP("example.test", AF_UNSPEC, &addrs);
    });
    CHECK(cotest::WaitUntil([&]() { return server.names.size() == 2; }));
    int64_t start = (int64_t)st_utime();
    st_thread_interrupt(lookup);
    st_thread_join(lookup, NULL);
    CHECK_EQ(ret, ERROR_THREAD_INTERRUPED);
    CHECK((int64_t)st_utime() - start < 100 * 1000);

    const int tcp_port = 19364;
    FakeDns udp(tcp_port, [](const DnsMessage &, const std::string &raw) {
        return std::vector<std::string>{Reply(raw, kDnsRcodeNoError, {}, true)};
    });
    CHECK(udp.ok());
    bool tcp_started = false;
    TcpServer tcp([&tcp_started](StreamConn &) {
        tcp_started = true;
        while (st_usleep(25 * 1000) == 0) {
        }
        return COCO_SUCCESS;
    });
    CHECK_EQ(tcp.Start(kLoopback, tcp_port), COCO_SUCCESS);

    int tcp_ret = -1;
    st_thread_t tcp_lookup = cotest::Go([&]() {
        Resolver resolver(ConfigFor(tcp_port, 5 * 1000 * 1000));
        std::vector<IpAddress> addrs;
        tcp_ret = resolver.LookupIP("example.test", AF_INET, &addrs);
    });
    bool tcp_ready = cotest::WaitUntil([&]() { return tcp_started; });
    int64_t tcp_start = (int64_t)st_utime();
    st_thread_interrupt(tcp_lookup);
    st_thread_join(tcp_lookup, NULL);
    int64_t tcp_elapsed = (int64_t)st_utime() - tcp_start;
    tcp.Stop();
    CHECK(tcp_ready);
    CHECK_EQ(udp.names.size(), 1);
    CHECK_EQ(tcp_ret, ERROR_THREAD_INTERRUPED);
    CHECK(tcp_elapsed < kShortTimeoutUs);
}

COTEST(DnsDialTcpByName) {
    const int port = 19354;
    TcpServer server([](StreamConn &conn) {
        conn.Write((void *)"hi", 2, nullptr);
        return 0;
    });
    CHECK_EQ(server.Start(kLoopback, port), COCO_SUCCESS);

    // localhost comes from /etc/hosts, as getaddrinfo would have found it.
    std::unique_ptr<TcpConn> conn;
    CHECK_EQ(DialTcp("localhost", port, 1000 * 1000, &conn), COCO_SUCCESS);
    if (conn) {
        char buf[2];
        conn->SetTimeout(1000 * 1000);
        CHECK_EQ(conn->ReadFully(buf, 2, nullptr), COCO_SUCCESS);
    }
    std::vector<std::string> addrs;
    CHECK_EQ(LookupHost("localhost", &addrs), COCO_SUCCESS);
    CHECK(!addrs.empty());
    server.Stop();
}
