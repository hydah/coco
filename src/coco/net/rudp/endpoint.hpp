#pragma once

#include <sys/socket.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "coco/base/owner_thread.hpp"
#include "coco/base/st_fwd.hpp"
#include "coco/base/task_group.hpp"
#include "coco/net/conn.hpp"
#include "coco/net/rudp/codec/control.hpp"

namespace coco {

class RudpConn;
class RudpListener;

// One connection of an endpoint. The endpoint owns it; a RudpConn handle points at it from
// the moment it is handed out until the handle is destroyed.
struct RudpEntry {
    RudpEntry() = default;
    ~RudpEntry();
    RudpEntry(const RudpEntry &) = delete;
    RudpEntry &operator=(const RudpEntry &) = delete;

    std::unique_ptr<RudpControl> ctl;
    sockaddr_storage peer;
    socklen_t peer_len = 0;
    std::string key;
    // A handle (or the dial in progress) owns it: only that may erase it.
    bool handed_out = false;
    // Broadcast on every change a waiting Read, Write, Close or dial may care about.
    st_cond_t changed = nullptr;
};

// One UDP socket and the coroutine (the pump) that reads it, runs the timers of every
// connection on it and hands each packet to its connection. Owned together by its handles
// (RudpListener, RudpConn) through shared_ptr; the pump holds none, and the destructor
// stops and waits for it before the socket closes. See .harness/docs/rudp.md, section 11.
// Library internal; belongs to the thread that created it.
class RudpEndpoint {
 public:
    // A client endpoint with one connection in SYN_SENT; the SYN is already out.
    static int Dial(const std::string &host, int port, const RudpOptions &options,
                    std::shared_ptr<RudpEndpoint> *ep, RudpEntry **entry);
    static int Listen(const std::string &ip, int port, const RudpOptions &options,
                      std::shared_ptr<RudpEndpoint> *ep);
    ~RudpEndpoint();

    RudpEndpoint(const RudpEndpoint &) = delete;
    RudpEndpoint &operator=(const RudpEndpoint &) = delete;

    // Sends what the entry's control has produced; may yield.
    void Transmit(RudpEntry *e);
    // Waits for e->changed until deadline_us (kNoTimeout: no deadline). Returns
    // COCO_SUCCESS (woken, maybe for nothing), ERROR_SOCKET_TIMEOUT once the deadline has
    // passed, never before, or ERROR_THREAD_INTERRUPED.
    int Wait(RudpEntry *e, int64_t deadline_us);
    // Removes an entry that was handed out. No coroutine may wait on it.
    void Erase(RudpEntry *e);

    // The next established connection, handed out; ERROR_THREAD_INTERRUPED or, after
    // CloseListener(), ERROR_RUDP_CLOSED.
    int Accept(RudpEntry **e);
    // Refuses new connections from now on and resets those not handed out.
    void CloseListener();

    std::string LocalAddr();
    const RudpOptions &options() const { return options_; }
    // Tests only: called after each datagram is handled, before the pump yields.
    void SetInputHook(std::function<void()> hook) { input_hook_ = std::move(hook); }

 private:
    struct Datagram {
        sockaddr_storage to;
        socklen_t to_len;
        std::string data;
    };

    RudpEndpoint(std::unique_ptr<DatagramConn> sock, bool server, const RudpOptions &options);
    int StartPump();
    int Pump();
    void HandleDatagram(size_t size, const sockaddr_storage &from, socklen_t from_len);
    void Tick(int64_t now);
    // Moves the control's output into *out, addressed to the entry's peer.
    void Collect(RudpEntry *e, std::vector<Datagram> *out);
    // Erases a closed entry nobody holds.
    void ReapIfDead(RudpEntry *e);
    void SendAll(const std::vector<Datagram> &out);

    // Destroyed after pump_, which the destructor stops first anyway.
    std::unique_ptr<DatagramConn> sock_;
    bool server_;
    RudpOptions options_;
    // The client's peer.
    sockaddr_storage peer_;
    socklen_t peer_len_ = 0;
    std::map<std::string, std::unique_ptr<RudpEntry>> entries_;
    std::deque<RudpEntry *> accept_queue_;
    // SYN_RCVD plus accept_queue_.
    size_t pending_ = 0;
    bool accepting_ = false;
    st_cond_t accept_cond_ = nullptr;
    std::vector<uint8_t> rbuf_;
    std::function<void()> input_hook_;
    OwnerThread owner_;
    TaskGroup pump_;
};

// Tests only.
void RudpSetInputHookForTest(RudpConn &conn, std::function<void()> hook);
void RudpSetInputHookForTest(RudpListener &l, std::function<void()> hook);

}  // namespace coco
