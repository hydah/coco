#include "coco/server/coco_tcp_server.hpp"

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
        std::unique_ptr<StreamConn> conn;
        int ret = server_->listener_->Accept(&conn);
        if (ret != COCO_SUCCESS) {
            if (ShouldTermCycle()) {
                break;
            }
            // Accept can keep failing (e.g. EMFILE) without ever blocking.
            coco_error("accept failed, retry later. ret=%d", ret);
            CocoSleepMs(10);
            continue;
        }

        Session *session = new Session(server_, std::move(conn));
        if (session->Start() != COCO_SUCCESS) {
            delete session;
        }
    }
    return COCO_SUCCESS;
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

    if (!options_.tls_key_file.empty() && !options_.tls_crt_file.empty()) {
        std::shared_ptr<TlsConfig> cfg;
        if ((ret = TlsConfig::NewServer(options_.tls_key_file, options_.tls_crt_file, &cfg)) !=
            COCO_SUCCESS) {
            coco_error("tcp server tls config failed. ret=%d", ret);
            return ret;
        }
        l.reset(new TlsListener(std::move(l), cfg));
    }

    listener_ = std::move(l);
    acceptor_ = new Acceptor(this);
    return acceptor_->Start();
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
    listener_.reset();
    manager_.Shutdown();

    stopped_ = true;
    NotifyShutdownWaiters();
}

}  // namespace coco
