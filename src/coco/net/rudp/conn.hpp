#pragma once

#include <functional>
#include <memory>
#include <string>

#include "coco/base/owner_thread.hpp"
#include "coco/net/conn.hpp"
#include "coco/net/rudp/codec/control.hpp"

namespace coco {

class RudpEndpoint;
struct RudpEntry;
class RudpListener;

// A reliable, ordered byte stream over UDP; see .harness/docs/rudp.md. It belongs to the
// thread that created it. One coroutine may read while others write; bytes of concurrent
// writers may interleave, as on a TcpConn.
//
// End of stream follows the StreamConn convention: once the peer's FIN has been read past,
// Read returns ERROR_SOCKET_READ with *nread == 0, as TcpConn does. ERROR_RUDP_RESET and
// ERROR_RUDP_TIMEOUT end the connection for good; ERROR_SOCKET_TIMEOUT from a Read or
// Write only ends that call.
class RudpConn : public StreamConn {
 public:
    // Close(), unless it has been called. May yield, at most for link_timeout_us; does not
    // wait once the coroutine has been stopped (CocoShouldStop()). No coroutine may be
    // blocked in Read or Write on this connection.
    ~RudpConn() override;

    int Read(void *buf, size_t size, ssize_t *nread) override;
    // Returns once every byte is in the send buffer, not when it was acknowledged; *nwrite
    // gets how many made it there, also on failure.
    int Write(void *buf, size_t size, ssize_t *nwrite) override;
    int Writev(const iovec *iov, int iov_size, ssize_t *nwrite) override;
    std::string LocalAddr() override;
    std::string RemoteAddr() override;
    // Bound one Read waiting for data, one Write waiting for buffer space.
    void SetRecvTimeout(int64_t timeout_us) override { recv_timeout_ = timeout_us; }
    void SetSendTimeout(int64_t timeout_us) override { send_timeout_ = timeout_us; }

    // Sends FIN after what was written and waits, at most link_timeout_us, until the peer
    // has acknowledged every byte. COCO_SUCCESS means all of it reached the peer's receive
    // buffer; an error means it may or may not have. Stopped (CocoShouldStop()) or
    // interrupted, it resets the connection and returns ERROR_THREAD_INTERRUPED. Later
    // calls return the same result; Read and Write, also those already waiting on other
    // coroutines, then fail with ERROR_RUDP_CLOSED.
    int Close();

    RudpStats Stats() const;

 private:
    friend class RudpListener;
    friend int DialRudp(const std::string &, int, int64_t, std::unique_ptr<RudpConn> *,
                        const RudpOptions &);
    friend void RudpSetInputHookForTest(RudpConn &conn, std::function<void()> hook);

    RudpConn(std::shared_ptr<RudpEndpoint> ep, RudpEntry *entry);

    std::shared_ptr<RudpEndpoint> ep_;
    RudpEntry *entry_;
    int64_t recv_timeout_ = kNoTimeout;
    int64_t send_timeout_ = kNoTimeout;
    bool closed_ = false;
    int close_ret_ = COCO_SUCCESS;
    OwnerThread owner_;
};

// Accepts RUDP connections on one UDP socket. The endpoint does the handshake, not Accept,
// so a silent peer holds up nothing. Destroying the listener refuses new connections (RST)
// and resets those not accepted yet; accepted ones keep working, and the UDP port stays
// bound until the last of them is destroyed. A TcpServer serves it with threads <= 1.
class RudpListener : public StreamListener {
 public:
    // No coroutine may be blocked in Accept.
    ~RudpListener() override;

    int Accept(std::unique_ptr<StreamConn> *conn) override;
    int AcceptRudp(std::unique_ptr<RudpConn> *conn);
    std::string Addr() override;

 private:
    friend int ListenRudp(const std::string &, int, std::unique_ptr<RudpListener> *,
                          const RudpOptions &);
    friend void RudpSetInputHookForTest(RudpListener &l, std::function<void()> hook);

    explicit RudpListener(std::shared_ptr<RudpEndpoint> ep) : ep_(std::move(ep)) {}

    std::shared_ptr<RudpEndpoint> ep_;
};

// ip must be an IP literal, as for ListenUdp.
int ListenRudp(const std::string &ip, int port, std::unique_ptr<RudpListener> *l,
               const RudpOptions &options = RudpOptions());
// Resolves host with DefaultResolver() and uses the first address. timeout_us bounds the
// handshake, not the name lookup; kNoTimeout means link_timeout_us. Fails with
// ERROR_RUDP_TIMEOUT when nothing answers, ERROR_RUDP_RESET when refused, and
// ERROR_THREAD_INTERRUPED when the coroutine is interrupted while waiting.
int DialRudp(const std::string &host, int port, int64_t timeout_us,
             std::unique_ptr<RudpConn> *conn, const RudpOptions &options = RudpOptions());
// DialRudp as a StreamDialer: for HttpClient::SetDialer, TlsDialer(cfg, RudpDialer()) ...
StreamDialer RudpDialer(RudpOptions options = RudpOptions());

}  // namespace coco
