#include "coco/net/rudp/conn.hpp"

#include "st.h"

#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/net/rudp/endpoint.hpp"

namespace coco {

namespace {

int64_t NowUs() { return (int64_t)st_utime(); }

int64_t DeadlineAfter(int64_t timeout_us) {
    return timeout_us == kNoTimeout ? kNoTimeout : NowUs() + timeout_us;
}

}  // namespace

RudpConn::RudpConn(std::shared_ptr<RudpEndpoint> ep, RudpEntry *entry)
    : ep_(std::move(ep)), entry_(entry) {}

RudpConn::~RudpConn() {
    owner_.Check();
    if (!closed_) {
        Close();
    }
    ep_->Erase(entry_);
    // May be the last reference: the endpoint then stops its pump and closes the socket.
    ep_.reset();
}

int RudpConn::Read(void *buf, size_t size, ssize_t *nread) {
    owner_.Check();
    if (nread) {
        *nread = 0;
    }
    if (size == 0) {
        return closed_ ? ERROR_RUDP_CLOSED : COCO_SUCCESS;
    }
    RudpControl &c = *entry_->ctl;
    int64_t deadline = DeadlineAfter(recv_timeout_);
    bool timed_out = false;
    while (true) {
        // Another coroutine may close the connection while this one waits.
        if (closed_) {
            return ERROR_RUDP_CLOSED;
        }
        // What arrived in order goes to the reader before any end of the stream.
        size_t n = c.Recv(buf, size);
        if (n > 0) {
            ep_->Transmit(entry_);  // a window update, if the window was closed
            if (nread) {
                *nread = (ssize_t)n;
            }
            return COCO_SUCCESS;
        }
        if (c.Eof()) {
            return ERROR_SOCKET_READ;
        }
        if (c.Error() != COCO_SUCCESS) {
            return c.Error();
        }
        if (timed_out) {
            return ERROR_SOCKET_TIMEOUT;
        }
        int ret = ep_->Wait(entry_, deadline);
        if (ret == ERROR_THREAD_INTERRUPED) {
            return ret;
        }
        // Data that came right at the deadline still counts: look once more.
        timed_out = ret == ERROR_SOCKET_TIMEOUT;
    }
}

int RudpConn::Write(void *buf, size_t size, ssize_t *nwrite) {
    iovec iov;
    iov.iov_base = buf;
    iov.iov_len = size;
    return Writev(&iov, 1, nwrite);
}

int RudpConn::Writev(const iovec *iov, int iov_size, ssize_t *nwrite) {
    owner_.Check();
    RudpControl &c = *entry_->ctl;
    int64_t deadline = DeadlineAfter(send_timeout_);
    size_t done = 0;
    int i = 0;
    size_t off = 0;
    bool timed_out = false;
    int ret = COCO_SUCCESS;
    while (true) {
        if (closed_) {
            ret = ERROR_RUDP_CLOSED;
            break;
        }
        if (c.Error() != COCO_SUCCESS) {
            ret = c.Error();
            break;
        }
        while (i < iov_size) {
            size_t len = iov[i].iov_len;
            size_t k = c.Send((const char *)iov[i].iov_base + off, len - off);
            done += k;
            off += k;
            if (off < len) {
                break;
            }
            ++i;
            off = 0;
        }
        c.Flush(NowUs());
        ep_->Transmit(entry_);
        if (i == iov_size) {
            break;
        }
        // Transmit may have yielded: what it waited for may have happened meanwhile.
        if (closed_ || c.Error() != COCO_SUCCESS || c.CanSend()) {
            continue;
        }
        if (timed_out) {
            ret = ERROR_SOCKET_TIMEOUT;
            break;
        }
        int w = ep_->Wait(entry_, deadline);
        if (w == ERROR_THREAD_INTERRUPED) {
            ret = w;
            break;
        }
        timed_out = w == ERROR_SOCKET_TIMEOUT;
    }
    if (nwrite) {
        *nwrite = (ssize_t)done;
    }
    return ret;
}

int RudpConn::Close() {
    owner_.Check();
    if (closed_) {
        return close_ret_;
    }
    closed_ = true;
    // A Read or Write waiting on another coroutine sees closed_ now.
    st_cond_broadcast(entry_->changed);
    RudpControl &c = *entry_->ctl;
    if (c.state() == RudpControl::State::kClosed) {
        close_ret_ = c.Error();
        return close_ret_;
    }
    // Stopped: a reset, not a FIN the peer would read as a clean end.
    if (CocoShouldStop()) {
        c.Abort();
        ep_->Transmit(entry_);
        close_ret_ = ERROR_THREAD_INTERRUPED;
        return close_ret_;
    }
    c.Close(NowUs());
    ep_->Transmit(entry_);
    // Bounded by link_timeout_us from here: the control ends the close by then.
    while (true) {
        if (c.CloseDone()) {
            close_ret_ = COCO_SUCCESS;
            break;
        }
        if (c.Error() != COCO_SUCCESS) {
            close_ret_ = c.Error();
            break;
        }
        // Stopping must not wait for the peer.
        if (CocoShouldStop() || ep_->Wait(entry_, kNoTimeout) == ERROR_THREAD_INTERRUPED) {
            c.Abort();
            ep_->Transmit(entry_);
            close_ret_ = ERROR_THREAD_INTERRUPED;
            break;
        }
    }
    return close_ret_;
}

std::string RudpConn::LocalAddr() { return ep_->LocalAddr(); }

std::string RudpConn::RemoteAddr() {
    return FormatSockaddr((const sockaddr *)&entry_->peer, entry_->peer_len);
}

RudpStats RudpConn::Stats() const { return entry_->ctl->Stats(); }

RudpListener::~RudpListener() { ep_->CloseListener(); }

int RudpListener::Accept(std::unique_ptr<StreamConn> *conn) {
    std::unique_ptr<RudpConn> c;
    int ret = AcceptRudp(&c);
    if (ret == COCO_SUCCESS) {
        *conn = std::move(c);
    }
    return ret;
}

int RudpListener::AcceptRudp(std::unique_ptr<RudpConn> *conn) {
    RudpEntry *e = nullptr;
    int ret = ep_->Accept(&e);
    if (ret == COCO_SUCCESS) {
        conn->reset(new RudpConn(ep_, e));
    }
    return ret;
}

std::string RudpListener::Addr() { return ep_->LocalAddr(); }

int ListenRudp(const std::string &ip, int port, std::unique_ptr<RudpListener> *l,
               const RudpOptions &options) {
    std::shared_ptr<RudpEndpoint> ep;
    int ret = RudpEndpoint::Listen(ip, port, options, &ep);
    if (ret == COCO_SUCCESS) {
        l->reset(new RudpListener(std::move(ep)));
    }
    return ret;
}

int DialRudp(const std::string &host, int port, int64_t timeout_us,
             std::unique_ptr<RudpConn> *conn, const RudpOptions &options) {
    std::shared_ptr<RudpEndpoint> ep;
    RudpEntry *e = nullptr;
    int ret = RudpEndpoint::Dial(host, port, options, &ep, &e);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    RudpControl &c = *e->ctl;
    int64_t deadline =
        NowUs() + (timeout_us == kNoTimeout ? ep->options().link_timeout_us : timeout_us);
    bool timed_out = false;
    while (true) {
        // A SYN_ACK that came right at the deadline still counts.
        if (c.Established()) {
            conn->reset(new RudpConn(ep, e));
            return COCO_SUCCESS;
        }
        if (c.Error() != COCO_SUCCESS) {
            ret = c.Error();
            break;
        }
        if (timed_out) {
            ret = ERROR_RUDP_TIMEOUT;
            break;
        }
        int w = ep->Wait(e, deadline);
        if (w == ERROR_THREAD_INTERRUPED) {
            ret = w;
            break;
        }
        timed_out = w == ERROR_SOCKET_TIMEOUT;
    }
    // Lets the server free its half-open slot now rather than at its link timeout.
    c.Abort();
    ep->Transmit(e);
    ep->Erase(e);
    return ret;
}

StreamDialer RudpDialer(RudpOptions options) {
    return [options](const std::string &host, int port, int64_t timeout_us,
                     std::unique_ptr<StreamConn> *conn) {
        std::unique_ptr<RudpConn> c;
        int ret = DialRudp(host, port, timeout_us, &c, options);
        if (ret == COCO_SUCCESS) {
            *conn = std::move(c);
        }
        return ret;
    };
}

}  // namespace coco
