#include "coco/net/rudp/endpoint.hpp"

#include <errno.h>
#include <string.h>

#include <algorithm>
#include <random>

#include "st.h"

#include "coco/coco_api.h"
#include "coco/log/log.hpp"
#include "coco/net/rudp/conn.hpp"
#include "coco/net/socket.hpp"
#include "coco/net/udp.hpp"
#include "coco/utils/utils.hpp"

namespace coco {

namespace {

// Datagrams the pump handles back to back before it lets the thread's other coroutines run.
constexpr int kPumpBatch = 64;

int64_t NowUs() { return (int64_t)st_utime(); }

uint32_t RandomU32() {
    static thread_local std::mt19937 gen(std::random_device{}());
    return (uint32_t)gen();
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

// The peer's address and port (and IPv6 scope), then conn_id.
std::string KeyOf(const sockaddr_storage &a, uint32_t conn_id) {
    std::string k;
    k.push_back((char)a.ss_family);
    if (a.ss_family == AF_INET) {
        const sockaddr_in *x = (const sockaddr_in *)&a;
        k.append((const char *)&x->sin_port, sizeof(x->sin_port));
        k.append((const char *)&x->sin_addr, sizeof(x->sin_addr));
    } else {
        const sockaddr_in6 *x = (const sockaddr_in6 *)&a;
        k.append((const char *)&x->sin6_port, sizeof(x->sin6_port));
        k.append((const char *)&x->sin6_addr, sizeof(x->sin6_addr));
        k.append((const char *)&x->sin6_scope_id, sizeof(x->sin6_scope_id));
    }
    k.append((const char *)&conn_id, sizeof(conn_id));
    return k;
}

std::string EncodeRst(uint32_t conn_id) {
    RudpPacket p;
    p.type = RudpType::kRst;
    p.conn_id = conn_id;
    std::string d;
    EncodeRudpPacket(p, &d);
    return d;
}

// What a waiter may be waiting for; the entry is signalled when it changes.
struct Signature {
    RudpControl::State state;
    bool can_send;
    bool readable;
    bool eof;

    explicit Signature(const RudpControl &c)
        : state(c.state()), can_send(c.CanSend()), readable(c.Readable()), eof(c.Eof()) {}
    bool operator!=(const Signature &o) const {
        return state != o.state || can_send != o.can_send || readable != o.readable ||
               eof != o.eof;
    }
};

}  // namespace

RudpEntry::~RudpEntry() {
    if (changed) {
        st_cond_destroy(changed);
    }
}

RudpEndpoint::RudpEndpoint(std::unique_ptr<DatagramConn> sock, bool server,
                           const RudpOptions &options)
    : sock_(std::move(sock)),
      server_(server),
      options_(ClampRudpOptions(options)),
      accepting_(server),
      rbuf_(kRudpMaxDatagram + 1) {
    memset(&peer_, 0, sizeof(peer_));
    // A full send buffer drops the datagram, as the network would; retransmission covers it.
    sock_->SetSendTimeout(0);
}

RudpEndpoint::~RudpEndpoint() {
    owner_.Check();
    // Before anything the pump uses goes away.
    pump_.Cancel();
    pump_.Wait();
    if (accept_cond_) {
        st_cond_destroy(accept_cond_);
    }
}

int RudpEndpoint::Dial(const std::string &host, int port, const RudpOptions &options,
                       std::shared_ptr<RudpEndpoint> *ep, RudpEntry **entry) {
    st_netfd_t stfd = nullptr;
    sockaddr_storage peer;
    socklen_t peer_len = 0;
    int ret = DialDatagram(host, port, &stfd, &peer, &peer_len);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    std::unique_ptr<DatagramConn> sock(new UdpConn(stfd, peer, peer_len));
    std::shared_ptr<RudpEndpoint> e(new RudpEndpoint(std::move(sock), false, options));
    e->peer_ = peer;
    e->peer_len_ = peer_len;

    uint32_t conn_id = 0;
    while (conn_id == 0) {
        conn_id = RandomU32();
    }
    std::unique_ptr<RudpEntry> en(new RudpEntry());
    if ((en->changed = st_cond_new()) == nullptr) {
        return ERROR_ST_CREATE_CYCLE_THREAD;
    }
    en->ctl.reset(new RudpControl(e->options_, conn_id, RandomU32(), NowUs()));
    en->peer = peer;
    en->peer_len = peer_len;
    en->key = KeyOf(peer, conn_id);
    en->handed_out = true;
    RudpEntry *raw = en.get();
    e->entries_[raw->key] = std::move(en);

    // The entry is in place before the pump first runs, so it ticks from the start.
    if ((ret = e->StartPump()) != COCO_SUCCESS) {
        return ret;
    }
    e->Transmit(raw);
    *ep = std::move(e);
    *entry = raw;
    return COCO_SUCCESS;
}

int RudpEndpoint::Listen(const std::string &ip, int port, const RudpOptions &options,
                         std::shared_ptr<RudpEndpoint> *ep) {
    std::unique_ptr<UdpListener> l;
    int ret = ListenUdp(ip, port, &l);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    std::shared_ptr<RudpEndpoint> e(new RudpEndpoint(std::move(l), true, options));
    if ((e->accept_cond_ = st_cond_new()) == nullptr) {
        return ERROR_ST_CREATE_CYCLE_THREAD;
    }
    if ((ret = e->StartPump()) != COCO_SUCCESS) {
        return ret;
    }
    *ep = std::move(e);
    return COCO_SUCCESS;
}

int RudpEndpoint::StartPump() {
    return pump_.Spawn([this]() { return Pump(); });
}

int RudpEndpoint::Pump() {
    int64_t next_tick = NowUs() + options_.interval_us;
    int handled = 0;
    while (!CocoShouldStop()) {
        int64_t now = NowUs();
        sock_->SetRecvTimeout(entries_.empty() ? kNoTimeout : std::max<int64_t>(next_tick - now, 0));
        sockaddr_storage from;
        int from_len = sizeof(from);
        ssize_t n = 0;
        int ret = sock_->RecvFrom(rbuf_.data(), (int)rbuf_.size(), &n, (sockaddr *)&from, &from_len);
        if (ret == COCO_SUCCESS) {
            HandleDatagram((size_t)n, from, (socklen_t)from_len);
            if (++handled % kPumpBatch == 0) {
                CocoYield();
            }
        } else if (ret != ERROR_SOCKET_TIMEOUT) {
            if (CocoShouldStop()) {
                break;
            }
            // An error that does not block must not spin the thread.
            coco_warn("rudp: recvfrom failed. ret=%d, errno=%d", ret, errno);
            CocoSleepMs(10);
        }
        now = NowUs();
        if (now >= next_tick) {
            Tick(now);
            next_tick = NowUs() + options_.interval_us;
        }
    }
    return COCO_SUCCESS;
}

void RudpEndpoint::HandleDatagram(size_t size, const sockaddr_storage &from, socklen_t from_len) {
    RudpPacket p;
    if (!DecodeRudpPacket(rbuf_.data(), size, &p)) {
        return;
    }
    if (!server_ && !SameEndpoint(from, peer_)) {
        return;
    }
    std::vector<Datagram> out;
    RudpEntry *e = nullptr;
    std::string key = KeyOf(from, p.conn_id);
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        e = it->second.get();
        bool was_pending = e->ctl->state() == RudpControl::State::kSynRcvd;
        e->ctl->Input(p, NowUs());
        if (was_pending && e->ctl->Established()) {
            accept_queue_.push_back(e);
            st_cond_broadcast(accept_cond_);
        }
    } else if (server_ && p.type == RudpType::kSyn && accepting_) {
        if (pending_ >= (size_t)options_.backlog) {
            return;  // the client sends SYN again
        }
        std::unique_ptr<RudpEntry> en(new RudpEntry());
        if ((en->changed = st_cond_new()) == nullptr) {
            return;
        }
        en->ctl.reset(new RudpControl(options_, p, RandomU32(), NowUs()));
        memcpy(&en->peer, &from, from_len);
        en->peer_len = from_len;
        en->key = key;
        e = en.get();
        entries_[key] = std::move(en);
        ++pending_;
    } else if (p.type != RudpType::kRst) {
        Datagram d;
        memcpy(&d.to, &from, from_len);
        d.to_len = from_len;
        d.data = EncodeRst(p.conn_id);
        out.push_back(std::move(d));
    }
    if (e) {
        Collect(e, &out);
        st_cond_broadcast(e->changed);
        ReapIfDead(e);
    }
    if (input_hook_) {
        input_hook_();
    }
    // e may be gone once this yields.
    SendAll(out);
}

void RudpEndpoint::Tick(int64_t now) {
    std::vector<Datagram> out;
    std::vector<RudpEntry *> live;
    live.reserve(entries_.size());
    for (auto &kv : entries_) {
        live.push_back(kv.second.get());
    }
    for (RudpEntry *e : live) {
        Signature before(*e->ctl);
        e->ctl->Update(now);
        Collect(e, &out);
        if (Signature(*e->ctl) != before) {
            st_cond_broadcast(e->changed);
        }
        ReapIfDead(e);
    }
    SendAll(out);
}

void RudpEndpoint::Collect(RudpEntry *e, std::vector<Datagram> *out) {
    for (std::string &d : e->ctl->TakeOutput()) {
        Datagram g;
        memcpy(&g.to, &e->peer, e->peer_len);
        g.to_len = e->peer_len;
        g.data = std::move(d);
        out->push_back(std::move(g));
    }
}

void RudpEndpoint::ReapIfDead(RudpEntry *e) {
    if (e->handed_out || e->ctl->state() != RudpControl::State::kClosed) {
        return;
    }
    auto q = std::find(accept_queue_.begin(), accept_queue_.end(), e);
    if (q != accept_queue_.end()) {
        accept_queue_.erase(q);
    }
    --pending_;
    std::string key = e->key;
    entries_.erase(key);
}

void RudpEndpoint::SendAll(const std::vector<Datagram> &out) {
    bool interrupted = false;
    for (const Datagram &d : out) {
        ssize_t n = 0;
        // Failures count as loss. A full buffer makes st_sendto poll, which takes the
        // coroutine's interrupt if one is pending.
        if (sock_->SendTo((void *)d.data.data(), (int)d.data.size(), &n, (sockaddr *)&d.to,
                          (int)d.to_len) != COCO_SUCCESS &&
            errno == EINTR) {
            interrupted = true;
        }
    }
    // Handed back, so the next blocking call of the caller still sees it.
    if (interrupted) {
        st_thread_interrupt(st_thread_self());
    }
}

void RudpEndpoint::Transmit(RudpEntry *e) {
    owner_.Check();
    std::vector<Datagram> out;
    Collect(e, &out);
    SendAll(out);
}

int RudpEndpoint::Wait(RudpEntry *e, int64_t deadline_us) {
    owner_.Check();
    st_utime_t timeout = ST_UTIME_NO_TIMEOUT;
    if (deadline_us != kNoTimeout) {
        timeout = (st_utime_t)std::max<int64_t>(deadline_us - NowUs(), 0);
    }
    if (st_cond_timedwait(e->changed, timeout) == 0) {
        return COCO_SUCCESS;
    }
    if (errno != ETIME) {
        return ERROR_THREAD_INTERRUPED;
    }
    // ST counts the timeout from the clock it last read, which may be well before this call:
    // a wait that ends before the deadline is only a wake-up, and the caller waits again.
    return NowUs() < deadline_us ? COCO_SUCCESS : ERROR_SOCKET_TIMEOUT;
}

void RudpEndpoint::Erase(RudpEntry *e) {
    owner_.Check();
    std::string key = e->key;
    entries_.erase(key);
}

int RudpEndpoint::Accept(RudpEntry **e) {
    owner_.Check();
    while (true) {
        if (!accept_queue_.empty()) {
            RudpEntry *front = accept_queue_.front();
            accept_queue_.pop_front();
            --pending_;
            front->handed_out = true;
            *e = front;
            return COCO_SUCCESS;
        }
        if (!accepting_) {
            return ERROR_RUDP_CLOSED;
        }
        if (st_cond_wait(accept_cond_) != 0) {
            return ERROR_THREAD_INTERRUPED;
        }
    }
}

void RudpEndpoint::CloseListener() {
    owner_.Check();
    accepting_ = false;
    std::vector<Datagram> out;
    for (auto it = entries_.begin(); it != entries_.end();) {
        RudpEntry *e = it->second.get();
        if (e->handed_out) {
            ++it;
            continue;
        }
        e->ctl->Abort();
        Collect(e, &out);
        it = entries_.erase(it);
    }
    accept_queue_.clear();
    pending_ = 0;
    st_cond_broadcast(accept_cond_);
    // Erased before sending: the pump may run while this yields.
    SendAll(out);
}

std::string RudpEndpoint::LocalAddr() { return sock_->LocalAddr(); }

void RudpSetInputHookForTest(RudpConn &conn, std::function<void()> hook) {
    conn.ep_->SetInputHook(std::move(hook));
}

void RudpSetInputHookForTest(RudpListener &l, std::function<void()> hook) {
    l.ep_->SetInputHook(std::move(hook));
}

}  // namespace coco
