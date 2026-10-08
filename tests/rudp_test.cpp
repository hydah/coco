// RUDP: the packet codec, the protocol state machine over an in-memory link with a fake
// clock, and connections over the loopback, which run on the same thread as the test.

#include <string.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "st.h"

#include "coco/app/http/client.hpp"
#include "coco/app/http/mux.hpp"
#include "coco/app/http/server.hpp"
#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/net/rudp/codec/control.hpp"
#include "coco/net/rudp/codec/packet.hpp"
#include "coco/net/rudp/conn.hpp"
#include "coco/net/rudp/endpoint.hpp"
#include "coco/net/tcp_server.hpp"
#include "coco/net/tls/conn.hpp"
#include "coco/net/udp.hpp"
#include "test_util.hpp"

using namespace coco;

namespace {

const int64_t kMs = 1000;

std::string Encode(const RudpPacket &p) {
    std::string out;
    EncodeRudpPacket(p, &out);
    return out;
}

bool Decode(const std::string &d, RudpPacket *p) {
    return DecodeRudpPacket((const uint8_t *)d.data(), d.size(), p);
}

// What happens to one packet on the wire.
struct Fate {
    bool drop = false;
    int64_t extra_us = 0;  // delivered this much later than the others
    bool dup = false;      // delivered twice
};
Fate Drop() {
    Fate f;
    f.drop = true;
    return f;
}

// Two RudpControls joined by an in-memory link with a fake clock. Every step advances the
// clock by 1ms, delivers what is due and runs both timers. The server side is made from
// the first SYN that arrives, as the endpoint does. The client's SYN goes on the wire at
// the first step, so a filter set right after construction sees it.
class FakeLink {
 public:
    typedef std::function<Fate(bool to_server, const RudpPacket &p)> Filter;

    FakeLink(const RudpOptions &o, uint32_t client_isn = 1000, uint32_t server_isn = 5000)
        : opt_(o), server_isn_(server_isn) {
        client.reset(new RudpControl(o, 77, client_isn, now));
    }

    void Step() {
        now += kMs;
        for (int dir = 0; dir < 2; ++dir) {
            std::deque<Wire> &q = dir == 0 ? to_server_ : to_client_;
            std::deque<Wire> later;
            while (!q.empty()) {
                Wire w = q.front();
                q.pop_front();
                if (w.at > now) {
                    later.push_back(w);
                    continue;
                }
                Deliver(dir == 0, w.p);
            }
            for (const Wire &w : later) {
                q.push_back(w);
            }
        }
        client->Update(now);
        if (server) {
            server->Update(now);
        }
        Collect();
    }

    bool RunUntil(std::function<bool()> pred, int64_t max_us = 60 * 1000 * kMs) {
        int64_t end = now + max_us;
        while (!pred()) {
            if (now >= end) {
                return false;
            }
            Step();
        }
        return true;
    }

    // Sends what fits from *pending on the client, reads everything the server has.
    void Pump(std::string *pending, std::string *got) {
        if (!pending->empty()) {
            size_t n = client->Send(pending->data(), pending->size());
            pending->erase(0, n);
            client->Flush(now);
        }
        char buf[4096];
        size_t n;
        while (server && (n = server->Recv(buf, sizeof(buf))) > 0) {
            got->append(buf, n);
        }
        Collect();
    }

    // Moves data from client to server; false if it did not arrive in max_us.
    bool Transfer(const std::string &data, std::string *got, int64_t max_us = 60 * 1000 * kMs) {
        std::string pending = data;
        return RunUntil(
            [&]() {
                Pump(&pending, got);
                return got->size() >= data.size();
            },
            max_us);
    }

    void Collect() {
        Queue(true, client->TakeOutput());
        if (server) {
            Queue(false, server->TakeOutput());
        }
    }

    std::unique_ptr<RudpControl> client;
    std::unique_ptr<RudpControl> server;
    int64_t now = 0;
    int64_t delay_us = kMs;
    Filter filter;
    int servers_made = 0;
    // Every packet put on the wire, before the filter, with the time it was sent.
    std::vector<std::pair<int64_t, RudpPacket>> sent_to_server;
    std::vector<std::pair<int64_t, RudpPacket>> sent_to_client;

 private:
    struct Wire {
        int64_t at;
        RudpPacket p;
    };

    void Queue(bool to_server, std::vector<std::string> out) {
        for (const std::string &d : out) {
            RudpPacket p;
            CHECK(Decode(d, &p));
            (to_server ? sent_to_server : sent_to_client).push_back({now, p});
            Fate f = filter ? filter(to_server, p) : Fate();
            if (f.drop) {
                continue;
            }
            std::deque<Wire> &q = to_server ? to_server_ : to_client_;
            q.push_back({now + delay_us + f.extra_us, p});
            if (f.dup) {
                q.push_back({now + delay_us + f.extra_us, p});
            }
        }
    }

    void Deliver(bool to_server, const RudpPacket &p) {
        if (!to_server) {
            client->Input(p, now);
        } else if (server) {
            server->Input(p, now);
        } else if (p.type == RudpType::kSyn) {
            server.reset(new RudpControl(opt_, p, server_isn_, now));
            ++servers_made;
        }
        Collect();
    }

    RudpOptions opt_;
    uint32_t server_isn_;
    std::deque<Wire> to_server_;
    std::deque<Wire> to_client_;
};

std::string Pattern(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) {
        s[i] = (char)(i * 131 + i / 977);
    }
    return s;
}

bool BothEstablished(FakeLink &l) {
    return l.client->Established() && l.server && l.server->Established();
}

const char *kLoopback = "127.0.0.1";

// Small timers, so timeouts in the loopback cases take milliseconds.
RudpOptions Quick() {
    RudpOptions o;
    o.interval_us = 5 * kMs;
    o.initial_rto_us = 30 * kMs;
    o.min_rto_us = 20 * kMs;
    o.max_rto_us = 100 * kMs;
    o.link_timeout_us = 600 * kMs;
    return o;
}

int64_t NowUs() { return (int64_t)st_utime(); }

int Echo(StreamConn &conn) {
    char buf[8192];
    ssize_t n = 0;
    int ret;
    while ((ret = conn.Read(buf, sizeof(buf), &n)) == COCO_SUCCESS) {
        if ((ret = conn.Write(buf, (size_t)n, nullptr)) != COCO_SUCCESS) {
            break;
        }
    }
    return ret;
}

// Reads until size bytes came or a read fails; returns the last result.
int ReadN(StreamConn &conn, size_t size, std::string *out) {
    char buf[8192];
    while (out->size() < size) {
        ssize_t n = 0;
        int ret = conn.Read(buf, std::min(sizeof(buf), size - out->size()), &n);
        if (ret != COCO_SUCCESS) {
            return ret;
        }
        out->append(buf, (size_t)n);
    }
    return COCO_SUCCESS;
}

// Writes data on its own coroutine and reads the echo on the caller's.
bool EchoRoundTrip(RudpConn &c, const std::string &data) {
    int wret = -1;
    st_thread_t w = cotest::Go([&]() { wret = c.Write((void *)data.data(), data.size(), nullptr); });
    std::string got;
    int rret = ReadN(c, data.size(), &got);
    st_thread_join(w, NULL);
    CHECK_EQ(wret, COCO_SUCCESS);
    CHECK_EQ(rret, COCO_SUCCESS);
    return got == data;
}

// Forwards datagrams between a client-facing port and a server, on two coroutines of this
// thread. fate decides for each datagram: drop it, or hold it back until the next one in
// the same direction has gone (a reorder).
class LossyRelay {
 public:
    enum Action { kPass, kDrop, kHold };
    typedef std::function<Action(bool to_server, int index, const RudpPacket &p)> Rule;

    LossyRelay(int front_port, int server_port, Rule rule) : rule_(rule) {
        ok_ = ListenUdp(kLoopback, front_port, &front_) == COCO_SUCCESS &&
              DialUdp(kLoopback, server_port, kNoTimeout, &back_) == COCO_SUCCESS;
        if (ok_) {
            up_ = cotest::Go([this]() { Loop(true); });
            down_ = cotest::Go([this]() { Loop(false); });
        }
    }
    ~LossyRelay() {
        if (ok_) {
            stop_ = true;
            st_thread_interrupt(up_);
            st_thread_interrupt(down_);
            st_thread_join(up_, NULL);
            st_thread_join(down_, NULL);
        }
    }
    bool ok() const { return ok_; }
    void SetRule(Rule rule) { rule_ = rule; }
    int forwarded[2] = {0, 0};

 private:
    void Loop(bool to_server) {
        char buf[2048];
        std::string held;
        int index = 0;
        while (!stop_) {
            sockaddr_storage from;
            int from_len = sizeof(from);
            ssize_t n = 0;
            DatagramConn *in = to_server ? (DatagramConn *)front_.get() : (DatagramConn *)back_.get();
            if (in->RecvFrom(buf, sizeof(buf), &n, (sockaddr *)&from, &from_len) != COCO_SUCCESS) {
                continue;
            }
            if (to_server) {
                memcpy(&client_, &from, (size_t)from_len);
                client_len_ = from_len;
            }
            std::string d(buf, (size_t)n);
            RudpPacket p;
            DecodeRudpPacket((const uint8_t *)d.data(), d.size(), &p);
            Action a = rule_ ? rule_(to_server, index++, p) : kPass;
            if (a == kDrop) {
                continue;
            }
            if (a == kHold && held.empty()) {
                held = d;
                continue;
            }
            Send(to_server, d);
            if (!held.empty()) {
                Send(to_server, held);
                held.clear();
            }
        }
    }
    void Send(bool to_server, const std::string &d) {
        ssize_t n = 0;
        ++forwarded[to_server ? 0 : 1];
        if (to_server) {
            back_->Write((void *)d.data(), (int)d.size(), &n);
        } else {
            front_->SendTo((void *)d.data(), (int)d.size(), &n, (sockaddr *)&client_, client_len_);
        }
    }

    Rule rule_;
    bool ok_ = false;
    bool stop_ = false;
    std::unique_ptr<UdpListener> front_;
    std::unique_ptr<UdpConn> back_;
    sockaddr_storage client_;
    int client_len_ = 0;
    st_thread_t up_ = nullptr;
    st_thread_t down_ = nullptr;
};

}  // namespace

COTEST(RudpPacketRoundTrip) {
    RudpPacket p;
    p.type = RudpType::kData;
    p.window = 0x0102;
    p.conn_id = 0x03040506;
    p.seq = 0x0708090a;
    p.ack = 0xfffffffe;
    p.payload = "hello";
    std::string d = Encode(p);
    const std::string head("\x01\x04\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\xff\xff\xff\xfe", 16);
    CHECK(d == head + "hello");

    RudpPacket q;
    CHECK(Decode(d, &q));
    CHECK(q.type == RudpType::kData);
    CHECK_EQ(q.window, 0x0102);
    CHECK_EQ(q.conn_id, 0x03040506);
    CHECK_EQ(q.seq, 0x0708090a);
    CHECK_EQ(q.ack, 0xfffffffeu);
    CHECK(q.payload == "hello");

    for (RudpType t : {RudpType::kSyn, RudpType::kSynAck, RudpType::kAck, RudpType::kFin,
                       RudpType::kRst}) {
        RudpPacket c;
        c.type = t;
        c.conn_id = 7;
        c.seq = 1;
        std::string e = Encode(c);
        CHECK_EQ(e.size(), kRudpHeaderSize);
        RudpPacket r;
        CHECK(Decode(e, &r));
        CHECK(r.type == t);
        CHECK(r.payload.empty());
    }

    RudpPacket full;
    full.type = RudpType::kData;
    full.conn_id = 1;
    full.payload.assign(kRudpMss, 'x');
    CHECK(Decode(Encode(full), &q));
    CHECK_EQ(q.payload.size(), kRudpMss);

    CHECK(RudpBefore(0xffffffff, 0));
    CHECK(!RudpBefore(0, 0xffffffff));
    CHECK(!RudpBefore(5, 5));
}

COTEST(RudpPacketRejectsMalformed) {
    RudpPacket data;
    data.type = RudpType::kData;
    data.conn_id = 9;
    data.payload = "x";
    std::string good = Encode(data);
    RudpPacket p;
    CHECK(Decode(good, &p));

    CHECK(!Decode(std::string(), &p));
    CHECK(!Decode(good.substr(0, 15), &p));
    std::string too_long = good.substr(0, kRudpHeaderSize) + std::string(kRudpMss + 1, 'x');
    CHECK(!Decode(too_long, &p));

    std::string version = good;
    version[0] = 2;
    CHECK(!Decode(version, &p));
    for (int t : {0, 7, 255}) {
        std::string type = good;
        type[1] = (char)t;
        CHECK(!Decode(type, &p));
    }
    std::string no_id = good;
    no_id[4] = no_id[5] = no_id[6] = no_id[7] = 0;
    CHECK(!Decode(no_id, &p));

    CHECK(!Decode(good.substr(0, kRudpHeaderSize), &p));  // DATA without payload
    std::string ack_with_payload = good;
    ack_with_payload[1] = (char)RudpType::kAck;
    CHECK(!Decode(ack_with_payload, &p));
}

COTEST(RudpControlHandshakeLosses) {
    RudpOptions o;
    // The first SYN is lost: the client sends it again after the initial RTO.
    {
        FakeLink l(o);
        int syns = 0;
        l.filter = [&](bool to_server, const RudpPacket &p) {
            return to_server && p.type == RudpType::kSyn && ++syns == 1 ? Drop() : Fate();
        };
        CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
        CHECK_EQ(syns, 2);
        CHECK(l.now >= o.initial_rto_us);
        CHECK_EQ(l.servers_made, 1);
    }
    // The first SYN_ACK is lost: both sides resend, still one connection.
    {
        FakeLink l(o);
        int syn_acks = 0;
        l.filter = [&](bool to_server, const RudpPacket &p) {
            return !to_server && p.type == RudpType::kSynAck && ++syn_acks == 1 ? Drop() : Fate();
        };
        CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
        CHECK_EQ(l.servers_made, 1);
    }
    // The ACK completing the handshake is lost and the client has nothing to send, as when
    // the server speaks first: the server resends SYN_ACK, the client answers it again.
    {
        FakeLink l(o);
        int acks = 0;
        l.filter = [&](bool to_server, const RudpPacket &p) {
            return to_server && p.type == RudpType::kAck && ++acks == 1 ? Drop() : Fate();
        };
        CHECK(l.RunUntil([&]() { return l.client->Established(); }));
        CHECK(l.server && l.server->state() == RudpControl::State::kSynRcvd);
        CHECK(l.RunUntil([&]() { return l.server->Established(); }));
        CHECK_EQ(acks, 2);
        const std::string hello = "220 hello";
        CHECK_EQ(l.server->Send(hello.data(), hello.size()), hello.size());
        l.server->Flush(l.now);
        l.Collect();
        std::string got;
        CHECK(l.RunUntil([&]() {
            char buf[64];
            size_t n = l.client->Recv(buf, sizeof(buf));
            got.append(buf, n);
            return got == hello;
        }));
    }
    // Data from the client also completes the handshake when that ACK is lost.
    {
        FakeLink l(o);
        l.filter = [&](bool to_server, const RudpPacket &p) {
            return to_server && p.type == RudpType::kAck ? Drop() : Fate();
        };
        CHECK(l.RunUntil([&]() { return l.client->Established(); }));
        std::string got;
        CHECK(l.Transfer("abc", &got, 50 * kMs));
        CHECK(got == "abc");
        CHECK(l.server->Established());
    }
    // A stale SYN after the handshake: one SYN_ACK, answered by a pure ACK; nothing else.
    {
        FakeLink l(o);
        CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
        RudpPacket syn;
        CHECK(Decode(Encode(l.sent_to_server.front().second), &syn));
        CHECK(syn.type == RudpType::kSyn);
        size_t before = l.sent_to_client.size();
        size_t before_back = l.sent_to_server.size();
        l.server->Input(syn, l.now);
        l.Collect();
        CHECK(l.RunUntil([&]() { return l.sent_to_server.size() == before_back + 1; }, 10 * kMs));
        CHECK(l.sent_to_server.back().second.type == RudpType::kAck);
        CHECK_EQ(l.sent_to_client.size(), before + 1);
        CHECK(l.sent_to_client.back().second.type == RudpType::kSynAck);
        CHECK(BothEstablished(l));
        CHECK_EQ(l.servers_made, 1);
    }
    // No answer at all: the client gives up after link_timeout_us.
    {
        RudpOptions s = o;
        s.link_timeout_us = 1000 * kMs;
        FakeLink l(s);
        l.filter = [](bool, const RudpPacket &) { return Drop(); };
        CHECK(l.RunUntil([&]() { return l.client->Error() != COCO_SUCCESS; }));
        CHECK_EQ(l.client->Error(), ERROR_RUDP_TIMEOUT);
        CHECK_EQ(l.now, s.link_timeout_us);
        // A server may hold a half-open slot for it: the client says it gave up.
        CHECK(l.sent_to_server.back().second.type == RudpType::kRst);
    }
}

COTEST(RudpControlCloseBoundedByLinkTimeout) {
    RudpOptions o;
    o.recv_window = 4;
    o.link_timeout_us = 1000 * kMs;
    // The peer keeps answering but never reads: one segment more than its window can never
    // be delivered, so the close fails, link_timeout_us after it began.
    {
        FakeLink l(o);
        CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
        std::string data = Pattern(5 * kRudpMss);
        CHECK_EQ(l.server->Send(data.data(), data.size()), data.size());
        l.server->Flush(l.now);
        l.Collect();
        CHECK(l.RunUntil([&]() { return l.client->Stats().segments_received == 4; }));
        l.server->Close(l.now);
        int64_t closed_at = l.now;
        l.Collect();
        CHECK(l.RunUntil([&]() { return l.server->Error() != COCO_SUCCESS; }, 3 * o.link_timeout_us));
        CHECK_EQ(l.server->Error(), ERROR_RUDP_TIMEOUT);
        CHECK(l.now - closed_at >= o.link_timeout_us && l.now - closed_at <= o.link_timeout_us + 2 * kMs);
    }
    // Exactly a window's worth: the FIN needs no room, so it is taken and the close is done.
    {
        FakeLink l(o);
        CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
        std::string data = Pattern(4 * kRudpMss);
        CHECK_EQ(l.server->Send(data.data(), data.size()), data.size());
        l.server->Flush(l.now);
        l.server->Close(l.now);
        l.Collect();
        CHECK(l.RunUntil([&]() { return l.server->CloseDone(); }, 3 * o.link_timeout_us));
        std::string got;
        char buf[4096];
        size_t n;
        while ((n = l.client->Recv(buf, sizeof(buf))) > 0) {
            got.append(buf, n);
        }
        CHECK(got == data);
        CHECK(l.client->Eof());
    }
}

COTEST(RudpControlRingNotPowerOfTwo) {
    // 2^32 is no multiple of 3: with seq % 3 as the slot, 0xffffffff and 0 would share one.
    RudpOptions o;
    o.recv_window = 3;
    FakeLink l(o, 0xffffffffu);
    bool dropped = false;
    l.filter = [&](bool to_server, const RudpPacket &p) {
        if (to_server && p.type == RudpType::kData && p.seq == 0xffffffffu && !dropped) {
            dropped = true;
            return Drop();
        }
        return Fate();
    };
    std::string data = Pattern(4 * kRudpMss), got;
    CHECK(l.Transfer(data, &got, 10 * 1000 * kMs));
    CHECK(dropped);
    CHECK(got == data);
}

COTEST(RudpControlZeroWindowIsNotCongestion) {
    RudpOptions o;
    o.recv_window = 4;
    FakeLink l(o);
    CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
    std::string pending = Pattern(40 * kRudpMss), got;
    // The receiver does not read for 3 seconds; the probe times out again and again.
    for (int i = 0; i < 3000; ++i) {
        size_t n = l.client->Send(pending.data(), pending.size());
        pending.erase(0, n);
        l.client->Flush(l.now);
        l.Collect();
        l.Step();
    }
    CHECK(l.client->Stats().rto_resends > 0);
    // Flow control, not congestion: the window is not cut.
    CHECK_EQ(l.client->Stats().congestion_events, 0);
    CHECK(l.client->Stats().cwnd >= kRudpInitialCwnd);
    // Once the receiver reads, sending resumes at once, not at the probe's next timeout.
    uint64_t before = l.server->Stats().segments_received;
    int64_t read_at = l.now;
    char buf[4096];
    size_t n;
    while ((n = l.server->Recv(buf, sizeof(buf))) > 0) {
        got.append(buf, n);
    }
    l.Collect();
    CHECK(l.RunUntil([&]() { return l.server->Stats().segments_received > before; }));
    CHECK(l.now - read_at <= 10 * kMs);
}

COTEST(RudpControlNothingAfterFin) {
    RudpOptions o;
    FakeLink l(o, 1000);
    CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
    // DATA right after where the FIN will be, then that FIN: the stream ends at the FIN.
    RudpPacket d;
    d.type = RudpType::kData;
    d.conn_id = l.client->conn_id();
    d.seq = 1001;
    d.ack = 5000;
    d.payload = "after the end";
    l.server->Input(d, l.now);
    RudpPacket fin = d;
    fin.type = RudpType::kFin;
    fin.seq = 1000;
    fin.payload.clear();
    l.server->Input(fin, l.now);
    CHECK(l.server->Eof());
    CHECK(!l.server->Readable());
}

COTEST(RudpControlTransfersInOrder) {
    RudpOptions o;
    FakeLink l(o);
    std::string data = Pattern(1024 * 1024), got;
    CHECK(l.Transfer(data, &got));
    CHECK(got == data);
    RudpStats c = l.client->Stats();
    CHECK_EQ(c.rto_resends, 0);
    CHECK_EQ(c.fast_resends, 0);
    CHECK_EQ(c.segments_sent, (data.size() + kRudpMss - 1) / kRudpMss);
    CHECK(c.srtt_us >= 2 * kMs && c.srtt_us <= 4 * kMs);
    CHECK_EQ(l.server->Stats().duplicates, 0);
}

COTEST(RudpControlRecoversLoss) {
    RudpOptions o;
    FakeLink l(o);
    int n = 0;
    l.filter = [&](bool, const RudpPacket &p) {
        if (p.type == RudpType::kSyn || p.type == RudpType::kSynAck) {
            return Fate();
        }
        ++n;
        Fate f;
        if (n % 5 == 0) {
            f.drop = true;
        } else if (n % 7 == 0) {
            f.extra_us = 3 * kMs;  // overtaken by the next ones
        } else if (n % 11 == 0) {
            f.dup = true;
        }
        return f;
    };
    std::string data = Pattern(512 * 1024), got;
    CHECK(l.Transfer(data, &got));
    CHECK(got == data);
    CHECK(l.server->Stats().duplicates > 0);
    CHECK(l.client->Stats().fast_resends > 0);
}

COTEST(RudpControlRecoversBurstLoss) {
    RudpOptions o;
    FakeLink l(o, 1000);
    CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
    std::vector<int> ssthresh_seen;
    std::vector<uint32_t> lost_sent;
    l.filter = [&](bool to_server, const RudpPacket &p) {
        // The first transmission of segments 1020..1029 is lost.
        if (to_server && p.type == RudpType::kData && p.seq >= 1020 && p.seq < 1030) {
            for (uint32_t s : lost_sent) {
                if (s == p.seq) {
                    return Fate();
                }
            }
            lost_sent.push_back(p.seq);
            return Drop();
        }
        return Fate();
    };
    std::string data = Pattern(200 * kRudpMss), got, pending = data;
    int before = l.client->Stats().ssthresh;
    CHECK(l.RunUntil([&]() {
        l.Pump(&pending, &got);
        int s = l.client->Stats().ssthresh;
        if (s != (ssthresh_seen.empty() ? before : ssthresh_seen.back())) {
            ssthresh_seen.push_back(s);
        }
        return got.size() == data.size();
    }));
    CHECK(got == data);
    RudpStats c = l.client->Stats();
    CHECK(c.fast_resends >= 1);
    CHECK(c.fast_resends <= 10);
    CHECK_EQ(ssthresh_seen.size(), 1);
    CHECK_EQ(c.congestion_events, 1);
}

COTEST(RudpControlCongestionWindow) {
    RudpOptions o;
    o.send_window = 32;
    FakeLink l(o, 1000);
    CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
    CHECK_EQ(l.client->Stats().cwnd, kRudpInitialCwnd);
    CHECK_EQ(l.client->Stats().ssthresh, 32);

    // Slow start: each round trip (2ms here) doubles the window up to send_window, and the
    // segments in flight never exceed it.
    std::string pending = Pattern(400 * kRudpMss), got;
    std::vector<int> cwnds;
    bool within = true;
    CHECK(l.RunUntil([&]() {
        l.Pump(&pending, &got);
        RudpStats s = l.client->Stats();
        within = within && s.in_flight <= s.cwnd;
        if (cwnds.empty() || cwnds.back() != s.cwnd) {
            cwnds.push_back(s.cwnd);
        }
        return s.cwnd == 32;
    }));
    CHECK(within);
    CHECK(cwnds.size() >= 3);
    for (size_t i = 1; i < cwnds.size(); ++i) {
        CHECK(cwnds[i] > cwnds[i - 1] && cwnds[i] <= 2 * cwnds[i - 1]);
    }

    // One loss, recovered by fast retransmission: the window halves once. Its
    // retransmission is lost too, so the same segment then times out: the window drops to
    // one segment, and ssthresh, cut in this round already, stays.
    uint32_t victim = 0;
    int victim_sent = 0;
    l.filter = [&](bool to_server, const RudpPacket &p) {
        if (!to_server || p.type != RudpType::kData) {
            return Fate();
        }
        if (victim == 0) {
            victim = p.seq;
        }
        return p.seq == victim && ++victim_sent <= 2 ? Drop() : Fate();
    };
    int flight_at_loss = 0;
    CHECK(l.RunUntil([&]() {
        RudpStats s = l.client->Stats();
        if (s.fast_resends == 0) {
            flight_at_loss = s.in_flight;
        }
        l.Pump(&pending, &got);
        return l.client->Stats().fast_resends == 1;
    }));
    RudpStats after_fast = l.client->Stats();
    int expect = std::max(flight_at_loss / 2, kRudpMinSsthresh);
    CHECK(after_fast.ssthresh >= expect - 1 && after_fast.ssthresh <= expect + 1);
    // ACKs that arrived in the same step may have grown it by one in congestion avoidance.
    CHECK(after_fast.cwnd >= after_fast.ssthresh && after_fast.cwnd <= after_fast.ssthresh + 1);
    CHECK(l.RunUntil([&]() {
        l.Pump(&pending, &got);
        return l.client->Stats().rto_resends == 1;
    }));
    RudpStats after_rto = l.client->Stats();
    CHECK_EQ(after_rto.cwnd, 1);
    CHECK_EQ(after_rto.ssthresh, after_fast.ssthresh);
    CHECK_EQ(after_rto.congestion_events, 1);
    CHECK(l.RunUntil([&]() {
        l.Pump(&pending, &got);
        return pending.empty() && got.size() == 400 * kRudpMss;
    }));
}

COTEST(RudpControlKarnAndBackoff) {
    RudpOptions o;
    FakeLink l(o, 1000);
    CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
    int64_t handshake_srtt = l.client->Stats().srtt_us;
    CHECK(handshake_srtt > 0);

    // A segment sent once whose ACK is 50ms late: sampled, srtt moves.
    l.filter = [&](bool to_server, const RudpPacket &p) {
        Fate f;
        if (!to_server && p.type == RudpType::kAck) {
            f.extra_us = 50 * kMs;
        }
        return f;
    };
    std::string first;
    CHECK(l.Transfer("y", &first));
    CHECK(l.RunUntil([&]() { return l.client->Stats().in_flight == 0; }));
    int64_t srtt = l.client->Stats().srtt_us;
    CHECK(srtt > handshake_srtt + 5 * kMs);
    int64_t rto = l.client->Stats().rto_us;
    CHECK_EQ(rto, o.min_rto_us);

    // Sent four times, the ACK as late: not sampled (Karn), srtt stays.
    std::vector<int64_t> sent_at;
    l.filter = [&](bool to_server, const RudpPacket &p) {
        if (to_server && p.type == RudpType::kData) {
            sent_at.push_back(l.now);
            if (sent_at.size() <= 3) {
                return Drop();
            }
        }
        if (!to_server && p.type == RudpType::kAck) {
            Fate f;
            f.extra_us = 50 * kMs;  // a sample, if taken, would move srtt a lot
            return f;
        }
        return Fate();
    };
    std::string got;
    CHECK(l.Transfer("x", &got));
    CHECK(l.RunUntil([&]() { return l.client->Stats().in_flight == 0; }));
    CHECK_EQ(sent_at.size(), 4);
    CHECK_EQ(sent_at[1] - sent_at[0], rto);
    CHECK_EQ(sent_at[2] - sent_at[1], 2 * rto);
    CHECK_EQ(sent_at[3] - sent_at[2], 4 * rto);
    CHECK_EQ(l.client->Stats().srtt_us, srtt);
    CHECK_EQ(l.client->Stats().rto_resends, 3);
}

COTEST(RudpControlZeroWindowProbe) {
    RudpOptions o;
    o.recv_window = 4;
    o.link_timeout_us = 300 * kMs;
    FakeLink l(o);
    CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
    std::string data = Pattern(100 * kRudpMss), pending = data;
    // The server does not read for twice the link timeout.
    for (int i = 0; i < 600; ++i) {
        size_t n = l.client->Send(pending.data(), pending.size());
        pending.erase(0, n);
        l.client->Flush(l.now);
        l.Collect();
        l.Step();
    }
    CHECK_EQ(l.client->Error(), COCO_SUCCESS);
    CHECK_EQ(l.server->Stats().segments_received, 4);
    CHECK(l.client->Stats().rto_resends > 0);  // the probes

    // The window update sent on the first read is lost; a probe finds the space.
    bool dropped = false;
    l.filter = [&](bool to_server, const RudpPacket &p) {
        if (!to_server && p.type == RudpType::kAck && !dropped) {
            dropped = true;
            return Drop();
        }
        return Fate();
    };
    std::string got;
    CHECK(l.RunUntil([&]() {
        l.Pump(&pending, &got);
        return got.size() == data.size();
    }));
    CHECK(dropped);
    CHECK(got == data);
    CHECK_EQ(l.client->Error(), COCO_SUCCESS);
}

COTEST(RudpControlLinkTimeoutBoundary) {
    RudpOptions o;
    o.link_timeout_us = 300 * kMs;
    FakeLink l(o);
    CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
    l.filter = [](bool, const RudpPacket &) { return Drop(); };
    l.client->Send("x", 1);
    l.client->Flush(l.now);
    int64_t first_sent = l.now;
    l.Collect();
    CHECK(l.RunUntil([&]() { return l.now == first_sent + o.link_timeout_us - kMs; }));
    CHECK_EQ(l.client->Error(), COCO_SUCCESS);
    l.Step();
    CHECK_EQ(l.client->Error(), ERROR_RUDP_TIMEOUT);
    CHECK(l.sent_to_server.back().second.type == RudpType::kRst);
    // The server, with nothing outstanding, never times out.
    CHECK_EQ(l.server->Error(), COCO_SUCCESS);
}

COTEST(RudpControlSeqWraparound) {
    RudpOptions o;
    FakeLink l(o, 0xffffff00u, 0xfffffff0u);
    int n = 0;
    l.filter = [&](bool, const RudpPacket &p) {
        return p.type == RudpType::kData && ++n % 10 == 0 ? Drop() : Fate();
    };
    std::string data = Pattern(1000 * kRudpMss), got;
    CHECK(l.Transfer(data, &got));
    CHECK(got == data);
    // And back, across the server's wrap.
    std::string back = Pattern(300 * kRudpMss), got_back, pending = back;
    CHECK(l.RunUntil([&]() {
        if (!pending.empty()) {
            size_t k = l.server->Send(pending.data(), pending.size());
            pending.erase(0, k);
            l.server->Flush(l.now);
            l.Collect();
        }
        char buf[4096];
        size_t k;
        while ((k = l.client->Recv(buf, sizeof(buf))) > 0) {
            got_back.append(buf, k);
        }
        return got_back.size() == back.size();
    }));
    CHECK(got_back == back);
}

COTEST(RudpControlCloseRules) {
    RudpOptions o;
    // Our FIN acknowledged: done. The peer reads the data, then end of stream.
    {
        FakeLink l(o);
        std::string got;
        CHECK(l.Transfer("data", &got));
        l.client->Close(l.now);
        l.Collect();
        CHECK(!l.client->CloseDone());
        CHECK_EQ(l.client->Send("x", 1), 0);
        CHECK(l.RunUntil([&]() { return l.client->CloseDone(); }));
        CHECK(l.server->Eof());
        // The peer has our FIN, and closes at once: nothing of it is waiting.
        l.server->Close(l.now);
        CHECK(l.server->CloseDone());
        // Our closed side answers its FIN with RST.
        l.Collect();
        CHECK(l.RunUntil([&]() { return l.sent_to_server.back().second.type == RudpType::kRst; },
                         10 * kMs));
    }
    // The peer's RST after all our data was acknowledged: done, though the FIN was not.
    {
        FakeLink l(o);
        std::string got;
        CHECK(l.Transfer("data", &got));
        CHECK(l.RunUntil([&]() { return l.client->Stats().in_flight == 0; }));
        l.filter = [](bool to_server, const RudpPacket &) { return to_server ? Drop() : Fate(); };
        l.client->Close(l.now);
        l.Collect();
        RudpPacket rst;
        rst.type = RudpType::kRst;
        rst.conn_id = l.client->conn_id();
        l.client->Input(rst, l.now);
        CHECK(l.client->CloseDone());
        CHECK_EQ(l.client->Error(), COCO_SUCCESS);
    }
    // RST with data unacknowledged: reset, not done.
    {
        FakeLink l(o);
        CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
        l.filter = [](bool, const RudpPacket &) { return Drop(); };
        l.client->Send("data", 4);
        l.client->Flush(l.now);
        l.client->Close(l.now);
        RudpPacket rst;
        rst.type = RudpType::kRst;
        rst.conn_id = l.client->conn_id();
        l.client->Input(rst, l.now);
        CHECK(!l.client->CloseDone());
        CHECK_EQ(l.client->Error(), ERROR_RUDP_RESET);
    }
    // The peer's FIN came, but what we wrote after it is not acknowledged: not done. The
    // peer, closed already, answers it with RST, so the data may be lost: reset.
    {
        FakeLink l(o);
        CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
        l.client->Close(l.now);
        l.Collect();
        CHECK(l.RunUntil([&]() { return l.client->CloseDone() && l.server->Eof(); }));
        l.filter = [](bool to_server, const RudpPacket &) { return to_server ? Fate() : Drop(); };
        CHECK_EQ(l.server->Send("late", 4), 4);
        l.server->Flush(l.now);
        l.server->Close(l.now);
        l.Collect();
        CHECK(!l.server->CloseDone());
        l.filter = nullptr;
        CHECK(l.RunUntil([&]() { return l.server->Error() != COCO_SUCCESS; }));
        CHECK_EQ(l.server->Error(), ERROR_RUDP_RESET);
        CHECK(!l.server->CloseDone());
    }
    // Both close at once: each gets the other's FIN.
    {
        FakeLink l(o);
        CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
        l.client->Close(l.now);
        l.server->Close(l.now);
        l.Collect();
        CHECK(l.RunUntil([&]() { return l.client->CloseDone() && l.server->CloseDone(); },
                         100 * kMs));
    }
    // Abort: RST out, closed for good.
    {
        FakeLink l(o);
        CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
        l.client->Abort();
        l.Collect();
        CHECK_EQ(l.client->Error(), ERROR_RUDP_CLOSED);
        CHECK(l.RunUntil([&]() { return l.server->Error() == ERROR_RUDP_RESET; }, 10 * kMs));
    }
}

COTEST(RudpControlRejectsOutOfWindow) {
    RudpOptions o;
    o.recv_window = 8;
    FakeLink l(o, 1000);
    CHECK(l.RunUntil([&]() { return BothEstablished(l); }));
    l.client->Send("abc", 3);
    l.client->Flush(l.now);
    l.Collect();
    // Acknowledges a sequence number never sent: ignored as a whole.
    RudpPacket bogus;
    bogus.type = RudpType::kAck;
    bogus.conn_id = l.client->conn_id();
    bogus.seq = 1000;
    bogus.ack = 1005;
    bogus.window = 100;
    l.client->Input(bogus, l.now);
    CHECK_EQ(l.client->Stats().in_flight, 1);

    // A flood of data over the whole sequence space only fills the window.
    l.filter = [](bool, const RudpPacket &) { return Drop(); };
    uint64_t before = l.server->Stats().segments_received;
    RudpPacket d;
    d.type = RudpType::kData;
    d.conn_id = l.client->conn_id();
    d.ack = 5000;
    d.payload = "z";
    for (uint32_t i = 0; i < 3000; ++i) {
        d.seq = 1001 + i * 997;
        l.server->Input(d, l.now);
    }
    for (uint32_t i = 0; i < 20; ++i) {
        d.seq = 1001 + i;
        l.server->Input(d, l.now);
    }
    CHECK(l.server->Stats().segments_received - before <= 8);
    std::vector<std::string> out = l.server->TakeOutput();
    CHECK(out.size() == 3020);
    RudpPacket last;
    CHECK(Decode(out.back(), &last));
    CHECK(last.type == RudpType::kAck);
    CHECK_EQ(last.seq, last.ack - 1);  // a pure ACK: outside the window
}

COTEST(RudpEchoes) {
    const int port = 19401;
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l), COCO_SUCCESS);
    CHECK(l->Addr() == "127.0.0.1:19401");
    TcpServer server(Echo);
    CHECK_EQ(server.Start(std::move(l)), COCO_SUCCESS);

    std::unique_ptr<RudpConn> c;
    CHECK_EQ(DialRudp(kLoopback, port, 1000 * kMs, &c), COCO_SUCCESS);
    CHECK(c->RemoteAddr() == "127.0.0.1:19401");
    CHECK(EchoRoundTrip(*c, "hello"));
    CHECK(EchoRoundTrip(*c, Pattern(1024 * 1024)));
    CHECK_EQ(c->Close(), COCO_SUCCESS);
    CHECK_EQ(c->Read((void *)"", 0, nullptr), ERROR_RUDP_CLOSED);
    c.reset();
    CHECK(cotest::WaitUntil([&]() { return server.ConnCount() == 0; }));
    server.Stop();
}

COTEST(RudpThroughLossyRelay) {
    const int port = 19402, relay = 19403;
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, Quick()), COCO_SUCCESS);
    TcpServer server(Echo);
    CHECK_EQ(server.Start(std::move(l)), COCO_SUCCESS);
    // Every fifth datagram lost, every seventh overtaken by the next one, both ways.
    LossyRelay r(relay, port, [](bool, int i, const RudpPacket &) {
        if (i % 5 == 4) {
            return LossyRelay::kDrop;
        }
        return i % 7 == 3 ? LossyRelay::kHold : LossyRelay::kPass;
    });
    CHECK(r.ok());
    std::unique_ptr<RudpConn> c;
    CHECK_EQ(DialRudp(kLoopback, relay, 2000 * kMs, &c, Quick()), COCO_SUCCESS);
    CHECK(EchoRoundTrip(*c, Pattern(128 * 1024)));
    RudpStats s = c->Stats();
    CHECK(s.rto_resends + s.fast_resends > 0);
    CHECK(s.congestion_events > 0);
    CHECK_EQ(c->Close(), COCO_SUCCESS);
    c.reset();
    server.Stop();
}

COTEST(RudpDialTimesOut) {
    // Nothing listens on the port.
    std::unique_ptr<RudpConn> c;
    int64_t start = NowUs();
    CHECK_EQ(DialRudp(kLoopback, 19404, 200 * kMs, &c, Quick()), ERROR_RUDP_TIMEOUT);
    int64_t took = NowUs() - start;
    CHECK(took >= 200 * kMs && took < 1000 * kMs);
    CHECK(!c);
}

COTEST(RudpDialInterrupted) {
    const int port = 19405, relay = 19406;
    RudpOptions o = Quick();
    o.backlog = 1;
    // A half-open slot that the RST did not free would stay taken this long.
    o.link_timeout_us = 3000 * kMs;
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, o), COCO_SUCCESS);
    // The server's answers never come back, so the dial waits with a half-open slot taken.
    int syn_acks = 0;
    LossyRelay r(relay, port, [&](bool to_server, int, const RudpPacket &p) {
        if (!to_server && p.type == RudpType::kSynAck) {
            ++syn_acks;
        }
        return to_server ? LossyRelay::kPass : LossyRelay::kDrop;
    });
    CHECK(r.ok());
    int ret = 0;
    st_thread_t d = cotest::Go([&]() {
        std::unique_ptr<RudpConn> c;
        ret = DialRudp(kLoopback, relay, 5000 * kMs, &c, o);
    });
    CHECK(cotest::WaitUntil([&]() { return syn_acks > 0; }));
    int64_t start = NowUs();
    st_thread_interrupt(d);
    st_thread_join(d, NULL);
    CHECK_EQ(ret, ERROR_THREAD_INTERRUPED);
    CHECK(NowUs() - start < 500 * kMs);

    // Its RST freed the only slot at once, not at the link timeout: the next dial gets in.
    std::unique_ptr<RudpConn> c2;
    start = NowUs();
    CHECK_EQ(DialRudp(kLoopback, port, 5000 * kMs, &c2, o), COCO_SUCCESS);
    CHECK(NowUs() - start < 1000 * kMs);
    std::unique_ptr<RudpConn> a;
    CHECK_EQ(l->AcceptRudp(&a), COCO_SUCCESS);
}

COTEST(RudpReadTimeoutKeepsConn) {
    const int port = 19407;
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, Quick()), COCO_SUCCESS);
    // Answers "!" with "!" and swallows everything else.
    TcpServer server([](StreamConn &conn) {
        char buf[256];
        ssize_t n = 0;
        int ret;
        while ((ret = conn.Read(buf, sizeof(buf), &n)) == COCO_SUCCESS) {
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '!' && (ret = conn.Write((void *)"!", 1, nullptr)) != COCO_SUCCESS) {
                    return ret;
                }
            }
        }
        return ret;
    });
    CHECK_EQ(server.Start(std::move(l)), COCO_SUCCESS);
    std::unique_ptr<RudpConn> c;
    CHECK_EQ(DialRudp(kLoopback, port, 1000 * kMs, &c, Quick()), COCO_SUCCESS);

    // Another coroutine keeps writing, so the ACKs wake the reader again and again; the
    // read still times out once, 100ms after it started.
    bool stop = false;
    int writes = 0;
    st_thread_t w = cotest::Go([&]() {
        while (!stop) {
            c->Write((void *)"x", 1, nullptr);
            ++writes;
            CocoSleepMs(10);
        }
    });
    c->SetRecvTimeout(100 * kMs);
    char buf[16];
    ssize_t n = -1;
    int64_t start = NowUs();
    CHECK_EQ(c->Read(buf, sizeof(buf), &n), ERROR_SOCKET_TIMEOUT);
    int64_t took = NowUs() - start;
    stop = true;
    st_thread_join(w, NULL);
    CHECK_EQ(n, 0);
    CHECK(writes >= 5);
    // A deadline restarted on every wake-up would not end while the writer runs.
    CHECK(took >= 100 * kMs && took < 1000 * kMs);

    CHECK_EQ(c->Write((void *)"!", 1, nullptr), COCO_SUCCESS);
    c->SetRecvTimeout(1000 * kMs);
    CHECK_EQ(c->Read(buf, sizeof(buf), &n), COCO_SUCCESS);
    CHECK_EQ(n, 1);
    c.reset();
    server.Stop();
}

COTEST(RudpReadInterruptedKeepsData) {
    const int port = 19408;
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, Quick()), COCO_SUCCESS);
    std::unique_ptr<RudpConn> c, s;
    CHECK_EQ(DialRudp(kLoopback, port, 1000 * kMs, &c, Quick()), COCO_SUCCESS);
    CHECK_EQ(l->AcceptRudp(&s), COCO_SUCCESS);

    // The data is handed to the connection, which wakes the reader, and before the reader
    // runs it is interrupted too: the interrupt wins, the data stays.
    int ret = -1;
    ssize_t n = -1;
    st_thread_t reader = cotest::Go([&]() {
        char buf[16];
        ret = c->Read(buf, sizeof(buf), &n);
    });
    CocoSleepMs(5);
    bool fired = false;
    RudpSetInputHookForTest(*c, [&]() {
        if (!fired && c->Stats().segments_received > 0) {
            fired = true;
            st_thread_interrupt(reader);
        }
    });
    CHECK_EQ(s->Write((void *)"abc", 3, nullptr), COCO_SUCCESS);
    st_thread_join(reader, NULL);
    RudpSetInputHookForTest(*c, nullptr);
    CHECK(fired);
    CHECK_EQ(ret, ERROR_THREAD_INTERRUPED);
    CHECK_EQ(n, 0);
    char buf[16];
    ssize_t m = 0;
    CHECK_EQ(c->Read(buf, sizeof(buf), &m), COCO_SUCCESS);
    CHECK(std::string(buf, (size_t)m) == "abc");

    // Data already there when an interrupted coroutine reads: returned; the interrupt is
    // seen by the next read that blocks.
    CHECK_EQ(s->Write((void *)"def", 3, nullptr), COCO_SUCCESS);
    CHECK(cotest::WaitUntil([&]() { return c->Stats().segments_received == 2; }));
    int r1 = -1, r2 = -1;
    std::string got;
    st_thread_t t = cotest::Go([&]() {
        st_thread_interrupt(st_thread_self());
        char b[16];
        ssize_t k = 0;
        r1 = c->Read(b, sizeof(b), &k);
        got.assign(b, k > 0 ? (size_t)k : 0);
        r2 = c->Read(b, sizeof(b), &k);
    });
    st_thread_join(t, NULL);
    CHECK_EQ(r1, COCO_SUCCESS);
    CHECK(got == "def");
    CHECK_EQ(r2, ERROR_THREAD_INTERRUPED);
}

COTEST(RudpWriteBlocksOnWindow) {
    const int port = 19409;
    RudpOptions o = Quick();
    o.send_window = 8;
    o.recv_window = 8;
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, o), COCO_SUCCESS);
    std::unique_ptr<RudpConn> c, s;
    CHECK_EQ(DialRudp(kLoopback, port, 1000 * kMs, &c, o), COCO_SUCCESS);
    CHECK_EQ(l->AcceptRudp(&s), COCO_SUCCESS);

    // Nobody reads: the buffers fill and the write times out with part of it taken.
    c->SetSendTimeout(200 * kMs);
    std::string data = Pattern(1024 * 1024);
    ssize_t nw = -1;
    int64_t start = NowUs();
    CHECK_EQ(c->Write((void *)data.data(), data.size(), &nw), ERROR_SOCKET_TIMEOUT);
    int64_t took = NowUs() - start;
    CHECK(took >= 200 * kMs && took < 1000 * kMs);
    CHECK(nw > 0 && nw < (ssize_t)(32 * kRudpMss));

    // Once the peer reads, the rest goes through, and the stream is whole.
    std::string got;
    int rret = -1;
    st_thread_t r = cotest::Go([&]() { rret = ReadN(*s, data.size(), &got); });
    c->SetSendTimeout(kNoTimeout);
    ssize_t nw2 = 0;
    CHECK_EQ(c->Write((void *)(data.data() + nw), data.size() - (size_t)nw, &nw2), COCO_SUCCESS);
    CHECK_EQ(nw + nw2, (ssize_t)data.size());
    st_thread_join(r, NULL);
    CHECK_EQ(rret, COCO_SUCCESS);
    CHECK(got == data);
}

COTEST(RudpCloseBothOrders) {
    const int port = 19410;
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, Quick()), COCO_SUCCESS);
    std::string data = Pattern(100 * 1024);
    // The client closes first; the server reads it all, then the end, then closes.
    {
        std::unique_ptr<RudpConn> c, s;
        CHECK_EQ(DialRudp(kLoopback, port, 1000 * kMs, &c, Quick()), COCO_SUCCESS);
        CHECK_EQ(l->AcceptRudp(&s), COCO_SUCCESS);
        std::string got;
        int rret = -1, sret = -1;
        st_thread_t r = cotest::Go([&]() {
            rret = ReadN(*s, data.size() + 1, &got);
            sret = s->Close();
        });
        CHECK_EQ(c->Write((void *)data.data(), data.size(), nullptr), COCO_SUCCESS);
        CHECK_EQ(c->Close(), COCO_SUCCESS);
        st_thread_join(r, NULL);
        CHECK_EQ(rret, ERROR_SOCKET_READ);
        CHECK(got == data);
        CHECK_EQ(sret, COCO_SUCCESS);
    }
    // The server closes first; the client reads it all, then the end, every time.
    {
        std::unique_ptr<RudpConn> c, s;
        CHECK_EQ(DialRudp(kLoopback, port, 1000 * kMs, &c, Quick()), COCO_SUCCESS);
        CHECK_EQ(l->AcceptRudp(&s), COCO_SUCCESS);
        int sret = -1;
        st_thread_t w = cotest::Go([&]() {
            s->Write((void *)data.data(), data.size(), nullptr);
            sret = s->Close();
        });
        std::string got;
        CHECK_EQ(ReadN(*c, data.size() + 1, &got), ERROR_SOCKET_READ);
        CHECK(got == data);
        char b[4];
        ssize_t n = -1;
        CHECK_EQ(c->Read(b, sizeof(b), &n), ERROR_SOCKET_READ);
        CHECK_EQ(n, 0);
        CHECK_EQ(c->Close(), COCO_SUCCESS);
        st_thread_join(w, NULL);
        CHECK_EQ(sret, COCO_SUCCESS);
    }
    // Both at once.
    {
        std::unique_ptr<RudpConn> c, s;
        CHECK_EQ(DialRudp(kLoopback, port, 1000 * kMs, &c, Quick()), COCO_SUCCESS);
        CHECK_EQ(l->AcceptRudp(&s), COCO_SUCCESS);
        int sret = -1;
        st_thread_t t = cotest::Go([&]() { sret = s->Close(); });
        CHECK_EQ(c->Close(), COCO_SUCCESS);
        st_thread_join(t, NULL);
        CHECK_EQ(sret, COCO_SUCCESS);
    }
}

COTEST(RudpCloseDeliversAllThroughLoss) {
    const int port = 19411, relay = 19412;
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, Quick()), COCO_SUCCESS);
    int fins = 0;
    LossyRelay r(relay, port, [&](bool to_server, int i, const RudpPacket &p) {
        if (to_server && p.type == RudpType::kFin && ++fins == 1) {
            return LossyRelay::kDrop;
        }
        return !to_server && i % 4 == 3 ? LossyRelay::kDrop : LossyRelay::kPass;
    });
    CHECK(r.ok());
    std::unique_ptr<RudpConn> c, s;
    CHECK_EQ(DialRudp(kLoopback, relay, 2000 * kMs, &c, Quick()), COCO_SUCCESS);
    CHECK_EQ(l->AcceptRudp(&s), COCO_SUCCESS);
    std::string data = Pattern(1024 * 1024), got;
    int rret = -1;
    st_thread_t rd = cotest::Go([&]() { rret = ReadN(*s, data.size() + 1, &got); });
    CHECK_EQ(c->Write((void *)data.data(), data.size(), nullptr), COCO_SUCCESS);
    CHECK_EQ(c->Close(), COCO_SUCCESS);
    st_thread_join(rd, NULL);
    CHECK_EQ(rret, ERROR_SOCKET_READ);
    CHECK(got == data);
    CHECK(fins >= 2);
}

COTEST(RudpCloseTimesOutWhenPeerGone) {
    const int port = 19413, relay = 19414;
    RudpOptions o = Quick();
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, o), COCO_SUCCESS);
    bool cut = false;
    LossyRelay r(relay, port, [&](bool, int, const RudpPacket &) {
        return cut ? LossyRelay::kDrop : LossyRelay::kPass;
    });
    CHECK(r.ok());
    std::unique_ptr<RudpConn> c, s;
    CHECK_EQ(DialRudp(kLoopback, relay, 1000 * kMs, &c, o), COCO_SUCCESS);
    CHECK_EQ(l->AcceptRudp(&s), COCO_SUCCESS);
    cut = true;
    CHECK_EQ(c->Write((void *)"x", 1, nullptr), COCO_SUCCESS);
    int64_t start = NowUs();
    CHECK_EQ(c->Close(), ERROR_RUDP_TIMEOUT);
    int64_t took = NowUs() - start;
    CHECK(took >= o.link_timeout_us - 10 * kMs && took < o.link_timeout_us + 500 * kMs);
    CHECK_EQ(c->Close(), ERROR_RUDP_TIMEOUT);
}

COTEST(RudpServerStopResetsConns) {
    const int port = 19415;
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, Quick()), COCO_SUCCESS);
    bool in_handler = false;
    TcpServer server([&](StreamConn &conn) {
        in_handler = true;
        char b[16];
        ssize_t n = 0;
        int ret;
        while ((ret = conn.Read(b, sizeof(b), &n)) == COCO_SUCCESS) {
        }
        return ret;
    });
    CHECK_EQ(server.Start(std::move(l)), COCO_SUCCESS);
    std::unique_ptr<RudpConn> c;
    CHECK_EQ(DialRudp(kLoopback, port, 1000 * kMs, &c, Quick()), COCO_SUCCESS);
    CHECK_EQ(c->Write((void *)"hi", 2, nullptr), COCO_SUCCESS);
    CHECK(cotest::WaitUntil([&]() { return in_handler; }));
    int rret = -1;
    st_thread_t r = cotest::Go([&]() {
        char b[16];
        ssize_t n = 0;
        rret = c->Read(b, sizeof(b), &n);
    });
    CocoSleepMs(5);
    // The handler is stopped in its read; its connection resets instead of closing.
    int64_t start = NowUs();
    server.Stop();
    CHECK(NowUs() - start < 500 * kMs);
    st_thread_join(r, NULL);
    CHECK_EQ(rret, ERROR_RUDP_RESET);
    std::unique_ptr<RudpListener> again;
    CHECK_EQ(ListenRudp(kLoopback, port, &again, Quick()), COCO_SUCCESS);
}

COTEST(RudpListenerDestroyedKeepsAccepted) {
    const int port = 19416;
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, Quick()), COCO_SUCCESS);
    std::unique_ptr<RudpConn> ac, as, pc;
    CHECK_EQ(DialRudp(kLoopback, port, 1000 * kMs, &ac, Quick()), COCO_SUCCESS);
    CHECK_EQ(l->AcceptRudp(&as), COCO_SUCCESS);
    CHECK_EQ(DialRudp(kLoopback, port, 1000 * kMs, &pc, Quick()), COCO_SUCCESS);
    CocoSleepMs(20);
    l.reset();

    // Never accepted: reset.
    char b[16];
    ssize_t n = 0;
    pc->SetRecvTimeout(1000 * kMs);
    CHECK_EQ(pc->Read(b, sizeof(b), &n), ERROR_RUDP_RESET);
    // New connections are refused.
    std::unique_ptr<RudpConn> x;
    CHECK_EQ(DialRudp(kLoopback, port, 1000 * kMs, &x, Quick()), ERROR_RUDP_RESET);
    // The accepted one still works.
    CHECK_EQ(ac->Write((void *)"ping", 4, nullptr), COCO_SUCCESS);
    std::string got;
    CHECK_EQ(ReadN(*as, 4, &got), COCO_SUCCESS);
    CHECK(got == "ping");
    // The port stays bound until the last connection on it is gone.
    std::unique_ptr<RudpListener> again;
    CHECK(ListenRudp(kLoopback, port, &again, Quick()) != COCO_SUCCESS);
    as.reset();
    CHECK_EQ(ListenRudp(kLoopback, port, &again, Quick()), COCO_SUCCESS);
}

COTEST(RudpListenerBacklog) {
    const int port = 19417;
    RudpOptions o = Quick();
    o.backlog = 2;
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, o), COCO_SUCCESS);
    // Two SYNs that never complete the handshake take both slots.
    std::unique_ptr<UdpConn> raw;
    CHECK_EQ(DialUdp(kLoopback, port, kNoTimeout, &raw), COCO_SUCCESS);
    int64_t t0 = NowUs();
    for (uint32_t id : {11u, 12u}) {
        RudpPacket syn;
        syn.type = RudpType::kSyn;
        syn.window = 128;
        syn.conn_id = id;
        syn.seq = 1;
        std::string d = Encode(syn);
        ssize_t n = 0;
        CHECK_EQ(raw->Write((void *)d.data(), (int)d.size(), &n), COCO_SUCCESS);
    }
    CocoSleepMs(5);
    std::unique_ptr<RudpConn> c;
    CHECK_EQ(DialRudp(kLoopback, port, 200 * kMs, &c, o), ERROR_RUDP_TIMEOUT);
    // The half-open ones go after the link timeout; then there is room.
    CHECK_EQ(DialRudp(kLoopback, port, 2000 * kMs, &c, o), COCO_SUCCESS);
    CHECK(NowUs() - t0 >= o.link_timeout_us);
    std::unique_ptr<RudpConn> a;
    CHECK_EQ(l->AcceptRudp(&a), COCO_SUCCESS);
}

COTEST(RudpPumpYieldsUnderFlood) {
    const int port = 19418;
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, Quick()), COCO_SUCCESS);
    int handled = 0;
    RudpSetInputHookForTest(*l, [&]() { ++handled; });
    // From a blocking socket of its own, so the flood goes out without the pump running.
    std::unique_ptr<UdpConn> raw;
    CHECK_EQ(DialUdp(kLoopback, port, kNoTimeout, &raw), COCO_SUCCESS);
    RudpPacket ack;
    ack.type = RudpType::kAck;
    ack.conn_id = 99;
    std::string d = Encode(ack);
    const int kFlood = 150;
    for (int i = 0; i < kFlood; ++i) {
        ssize_t n = 0;
        raw->Write((void *)d.data(), (int)d.size(), &n);
    }
    CHECK_EQ(handled, 0);
    // Runs whenever it gets the thread, and notes how far the pump had come.
    std::vector<int> seen;
    bool done = false;
    st_thread_t side = cotest::Go([&]() {
        while (!done) {
            seen.push_back(handled);
            CocoYield();
        }
    });
    cotest::WaitUntil([&]() { return handled >= kFlood; }, 500);
    done = true;
    st_thread_join(side, NULL);
    int total = handled;
    CHECK(total > 64);
    bool between = false;
    for (int s : seen) {
        between = between || (s > 0 && s < total);
    }
    CHECK(between);
}

COTEST(RudpTcpServerThreadsRejected) {
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, 19422, &l, Quick()), COCO_SUCCESS);
    TcpServerOptions opt;
    opt.threads = 2;
    TcpServer server(Echo, opt);
    CHECK_EQ(server.Start(std::move(l)), ERROR_SYSTEM_CONFIG_INVALID);
}

COTEST(RudpHttpOverRudp) {
    const int port = 19419, raw_port = 19420;
    std::string big = Pattern(300 * 1024);
    HttpServeMux mux;
    mux.HandleFunc("/len", [](HttpResponseWriter &w, HttpRequest &) { w.Write("sized body"); });
    // Larger than the writer's buffer: chunked.
    mux.HandleFunc("/chunked", [&big](HttpResponseWriter &w, HttpRequest &) { w.Write(big); });
    HttpServeOptions so;
    so.read_timeout_us = 100 * kMs;  // idle keep-alive connections are closed soon
    TcpServer server([&](StreamConn &c) { return ServeHttpConn(c, &mux, so); });
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, Quick()), COCO_SUCCESS);
    CHECK_EQ(server.Start(std::move(l)), COCO_SUCCESS);

    HttpClient client;
    int dials = 0;
    StreamDialer rudp = RudpDialer(Quick());
    client.SetDialer([&](const std::string &host, int p, int64_t timeout_us,
                         std::unique_ptr<StreamConn> *conn) {
        ++dials;
        return rudp(host, p, timeout_us, conn);
    });
    std::unique_ptr<HttpResponse> resp;
    std::string body;
    CHECK_EQ(client.Get("http://127.0.0.1:19419/len", &resp), COCO_SUCCESS);
    CHECK_EQ(resp->status_code, 200);
    resp->body.ReadAll(&body);
    CHECK(body == "sized body");
    resp.reset();
    body.clear();
    CHECK_EQ(client.Get("http://127.0.0.1:19419/chunked", &resp), COCO_SUCCESS);
    resp->body.ReadAll(&body);
    CHECK(body == big);
    resp.reset();
    CHECK_EQ(dials, 1);

    // The server has closed the pooled connection meanwhile: its end of stream reads as
    // TCP's does, so the client retries on a new connection.
    CocoSleepMs(300);
    body.clear();
    CHECK_EQ(client.Get("http://127.0.0.1:19419/len", &resp), COCO_SUCCESS);
    resp->body.ReadAll(&body);
    CHECK(body == "sized body");
    resp.reset();
    CHECK_EQ(dials, 2);

    // A body that ends where the stream ends.
    TcpServer raw([](StreamConn &c) {
        char buf[1024];
        ssize_t n = 0;
        c.Read(buf, sizeof(buf), &n);
        std::string r = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nuntil the end";
        return c.Write((void *)r.data(), r.size(), nullptr);
    });
    std::unique_ptr<RudpListener> rl;
    CHECK_EQ(ListenRudp(kLoopback, raw_port, &rl, Quick()), COCO_SUCCESS);
    CHECK_EQ(raw.Start(std::move(rl)), COCO_SUCCESS);
    body.clear();
    CHECK_EQ(client.Get("http://127.0.0.1:19420/", &resp), COCO_SUCCESS);
    resp->body.ReadAll(&body);
    CHECK(body == "until the end");
    resp.reset();

    client.CloseIdleConnections();
    server.Stop();
    raw.Stop();
}

COTEST(RudpTlsOverRudp) {
    const int port = 19421;
    std::shared_ptr<TlsConfig> scfg, ccfg;
    CHECK_EQ(TlsConfig::NewServer(COCO_SOURCE_DIR "/examples/http-server/server.key",
                                  COCO_SOURCE_DIR "/examples/http-server/server.crt", &scfg),
             COCO_SUCCESS);
    CHECK_EQ(TlsConfig::NewClient(&ccfg), COCO_SUCCESS);
    TcpServer server(TlsHandler(scfg, Echo));
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, Quick()), COCO_SUCCESS);
    CHECK_EQ(server.Start(std::move(l)), COCO_SUCCESS);

    StreamDialer dial = TlsDialer(ccfg, RudpDialer(Quick()));
    std::unique_ptr<StreamConn> c;
    CHECK_EQ(dial(kLoopback, port, 2000 * kMs, &c), COCO_SUCCESS);
    std::string data = Pattern(256 * 1024), got;
    int wret = -1;
    st_thread_t w = cotest::Go([&]() { wret = c->Write((void *)data.data(), data.size(), nullptr); });
    CHECK_EQ(ReadN(*c, data.size(), &got), COCO_SUCCESS);
    st_thread_join(w, NULL);
    CHECK_EQ(wret, COCO_SUCCESS);
    CHECK(got == data);
    c.reset();
    server.Stop();
}

COTEST(RudpTimeoutsAreNotEarly) {
    // ST measures a timed wait from the clock it last read, so after a stretch without a
    // yield a plain wait ends early. A dial's and a read's budget still runs from the call.
    auto spin = [](int64_t us) {
        int64_t t0 = NowUs();
        while (NowUs() - t0 < us) {
        }
    };
    std::unique_ptr<RudpConn> c;
    spin(150 * kMs);
    int64_t start = NowUs();
    CHECK_EQ(DialRudp(kLoopback, 19423, 200 * kMs, &c, Quick()), ERROR_RUDP_TIMEOUT);
    CHECK(NowUs() - start >= 200 * kMs);

    const int port = 19424;
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, Quick()), COCO_SUCCESS);
    std::unique_ptr<RudpConn> s;
    CHECK_EQ(DialRudp(kLoopback, port, 1000 * kMs, &c, Quick()), COCO_SUCCESS);
    CHECK_EQ(l->AcceptRudp(&s), COCO_SUCCESS);
    c->SetRecvTimeout(200 * kMs);
    char b[4];
    ssize_t n = 0;
    spin(150 * kMs);
    start = NowUs();
    CHECK_EQ(c->Read(b, sizeof(b), &n), ERROR_SOCKET_TIMEOUT);
    CHECK(NowUs() - start >= 200 * kMs);
}

COTEST(RudpReadEndsWhenClosed) {
    const int port = 19425;
    std::unique_ptr<RudpListener> l;
    CHECK_EQ(ListenRudp(kLoopback, port, &l, Quick()), COCO_SUCCESS);
    std::unique_ptr<RudpConn> c, s;
    CHECK_EQ(DialRudp(kLoopback, port, 1000 * kMs, &c, Quick()), COCO_SUCCESS);
    CHECK_EQ(l->AcceptRudp(&s), COCO_SUCCESS);
    // One coroutine reads with no timeout while another closes the connection.
    int rret = -1;
    st_thread_t r = cotest::Go([&]() {
        char b[16];
        ssize_t n = 0;
        rret = c->Read(b, sizeof(b), &n);
    });
    CocoSleepMs(5);
    CHECK_EQ(c->Close(), COCO_SUCCESS);
    CHECK(cotest::WaitUntil([&]() { return rret != -1; }, 500));
    if (rret == -1) {
        st_thread_interrupt(r);
    }
    st_thread_join(r, NULL);
    CHECK_EQ(rret, ERROR_RUDP_CLOSED);
}
