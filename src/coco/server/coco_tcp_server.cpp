#include "coco/server/coco_tcp_server.hpp"

#include <unistd.h>

#include "coco/base/shutdown.hpp"
#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/log/log.hpp"
#include "coco/net/layer4/coco_tcp.hpp"
#include "coco/net/tls/coco_tls.hpp"

namespace coco {

class TcpServer::Acceptor : public ListenRoutine {
 public:
    explicit Acceptor(TcpServer *server) : server_(server) {}
    virtual ~Acceptor() { Stop(); }

    virtual int Cycle();

 private:
    TcpServer *server_;
};

class TcpServer::Session : public ConnRoutine {
 public:
    Session(TcpServer *server, std::unique_ptr<StreamConn> conn)
        : ConnRoutine(&server->manager_), server_(server), conn_(std::move(conn)) {}
    virtual ~Session() = default;

    virtual std::string GetRemoteAddr() { return conn_->RemoteAddr(); }

 protected:
    virtual int DoCycle();

 private:
    TcpServer *server_;
    std::unique_ptr<StreamConn> conn_;
};

int TcpServer::Acceptor::Cycle() {
    while (!ShouldTermCycle()) {
        int ret = server_->AcceptOne();
        if (ret != COCO_SUCCESS) {
            if (ShouldTermCycle()) {
                break;
            }
            // Accept can keep failing (e.g. EMFILE) without ever blocking.
            coco_error("accept failed, retry later. ret=%d", ret);
            CocoSleepMs(10);
        }
    }
    return COCO_SUCCESS;
}

int TcpServer::AcceptOne() {
    int ret = COCO_SUCCESS;
    if (workers_.empty()) {
        std::unique_ptr<StreamConn> conn;
        if ((ret = listener_->Accept(&conn)) != COCO_SUCCESS) {
            return ret;
        }
        Session *session = new Session(this, std::move(conn));
        if (session->Start() != COCO_SUCCESS) {
            delete session;
        }
        return ret;
    }

    std::unique_ptr<TcpConn> conn;
    if ((ret = tcp_listener_->AcceptTcp(&conn)) != COCO_SUCCESS) {
        return ret;
    }
    Dispatch(std::move(conn));
    return ret;
}

namespace {

// A released fd on its way to a worker. Closed unless the worker takes it, which it does
// not when the worker is stopping or cannot start a coroutine for it.
class PendingFd {
 public:
    explicit PendingFd(int fd) : fd_(fd) {}
    ~PendingFd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    int Take() {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

 private:
    int fd_;
};

}  // namespace

void TcpServer::Dispatch(std::unique_ptr<TcpConn> conn) {
    CocoThread *target = workers_[0].get();
    for (auto &w : workers_) {
        if (w->Load() < target->Load()) {
            target = w.get();
        }
    }

    int fd = -1;
    if (conn->Release(&fd) != COCO_SUCCESS) {
        return;
    }
    auto pending = std::make_shared<PendingFd>(fd);
    int ret = target->Post([this, pending]() { ServeFd(pending->Take()); });
    if (ret != COCO_SUCCESS) {
        coco_warn("tcp server worker refused a connection. ret=%d", ret);
    }
}

void TcpServer::ServeFd(int fd) {
    std::unique_ptr<TcpConn> tcp;
    if (TcpConnFromFd(fd, &tcp) != COCO_SUCCESS) {
        return;
    }
    std::unique_ptr<StreamConn> conn(tcp.release());
    if (tls_) {
        conn.reset(new TlsConn(std::move(conn), tls_));
    }
    conn->SetRecvTimeout(options_.recv_timeout_us);
    conn->SetSendTimeout(options_.send_timeout_us);
    coco_trace("[TRACE_ANCHOR] Connection, remote addr: %s", conn->RemoteAddr().c_str());
    int ret = handler_(*conn);
    if (ret != COCO_SUCCESS && !coco_is_client_gracefully_close(ret)) {
        coco_warn("connection ended. ret=%d", ret);
    }
}

int TcpServer::Session::DoCycle() {
    const TcpServerOptions &opt = server_->options_;
    conn_->SetRecvTimeout(opt.recv_timeout_us);
    conn_->SetSendTimeout(opt.send_timeout_us);
    return server_->handler_(*conn_);
}

TcpServer::TcpServer(StreamHandler handler, TcpServerOptions options)
    : handler_(handler), options_(options) {}

TcpServer::~TcpServer() {
    Stop();
    if (acceptor_) {
        delete acceptor_;
        acceptor_ = nullptr;
    }
    listener_.reset();
}

int TcpServer::ListenAndServe(const std::string &ip, int port) {
    int ret = Start(ip, port);
    if (ret == COCO_SUCCESS) {
        Wait();
    }
    return ret;
}

int TcpServer::Serve(std::unique_ptr<StreamListener> l) {
    int ret = Start(std::move(l));
    if (ret == COCO_SUCCESS) {
        Wait();
    }
    return ret;
}

int TcpServer::Start(const std::string &ip, int port) {
    std::unique_ptr<TcpListener> l;
    int ret = ListenTcp(ip, port, &l);
    if (ret != COCO_SUCCESS) {
        coco_error("tcp server listen failed. ep=%s:%d, ret=%d", ip.c_str(), port, ret);
        return ret;
    }
    return Start(std::move(l));
}

int TcpServer::Start(std::unique_ptr<StreamListener> l) {
    int ret = COCO_SUCCESS;

    if (acceptor_ != nullptr) {
        coco_error("tcp server already serving");
        return ERROR_THREAD_STARTED;
    }
    if (stopped_) {
        coco_error("tcp server already stopped");
        return ERROR_THREAD_DISPOSED;
    }

    bool threaded = options_.threads > 1;
    TcpListener *tcp = nullptr;
    if (threaded && (tcp = dynamic_cast<TcpListener *>(l.get())) == nullptr) {
        // Only a TCP connection's fd can move to another thread.
        coco_error("tcp server with threads needs a TcpListener; TLS goes in the options");
        return ERROR_SYSTEM_CONFIG_INVALID;
    }

    if (!options_.tls_key_file.empty() && !options_.tls_crt_file.empty()) {
        std::shared_ptr<TlsConfig> cfg;
        if ((ret = TlsConfig::NewServer(options_.tls_key_file, options_.tls_crt_file, &cfg)) !=
            COCO_SUCCESS) {
            coco_error("tcp server tls config failed. ret=%d", ret);
            return ret;
        }
        // A TLS session cannot change threads: the workers handshake themselves.
        if (threaded) {
            tls_ = cfg;
        } else {
            l.reset(new TlsListener(std::move(l), cfg));
        }
    }

    for (int i = 0; threaded && i < options_.threads; ++i) {
        std::unique_ptr<CocoThread> w(new CocoThread());
        if ((ret = w->Start()) != COCO_SUCCESS) {
            coco_error("tcp server worker failed to start. ret=%d", ret);
            workers_.clear();
            return ret;
        }
        workers_.push_back(std::move(w));
    }

    tcp_listener_ = tcp;
    listener_ = std::move(l);
    acceptor_ = new Acceptor(this);
    return acceptor_->Start();
}

size_t TcpServer::ConnCount() const {
    size_t n = manager_.Size();
    for (auto &w : workers_) {
        n += w->Load();
    }
    return n;
}

void TcpServer::Wait() {
    if (acceptor_ == nullptr) {
        return;
    }
    WaitForShutdownOr([this] { return stopped_; });
    Stop();
}

void TcpServer::Stop() {
    if (stopping_) {
        // Another coroutine is stopping the server. The listener and the acceptor are not
        // safe to touch before it is done, and neither is returning before the server is
        // down, as callers rely on that.
        WaitUntilNotified([this] { return stopped_; });
        return;
    }
    stopping_ = true;

    // The acceptor must be gone first, or it could start a connection after the shutdown,
    // and the listener cannot be closed under its blocked accept.
    if (acceptor_) {
        acceptor_->Stop();
    }
    // Nobody accepts any more: refuse new connections rather than leave them to hang in the
    // backlog until the server is destroyed, and free the port.
    tcp_listener_ = nullptr;
    listener_.reset();
    manager_.Shutdown();
    // Each interrupts its connections, waits for them and is joined.
    for (auto &w : workers_) {
        w->Stop();
    }

    stopped_ = true;
    NotifyShutdownWaiters();
}

}  // namespace coco
