#include "server/coco_tcp_server.hpp"

#include <memory>

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "net/tls/coco_ssl.hpp"

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
    Session(TcpServer *server, TcpConn *conn) : ConnRoutine(&server->manager_), server_(server) {
        conn_.reset(conn);
    }
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
        TcpConn *conn = server_->listener_->Accept();
        if (conn == nullptr) {
            if (ShouldTermCycle()) {
                break;
            }
            // Accept can keep failing (e.g. EMFILE) without ever blocking.
            coco_error("accept failed, retry later");
            CocoSleepMs(10);
            continue;
        }

        Session *session = new Session(server_, conn);
        if (session->Start() != COCO_SUCCESS) {
            delete session;
        }
    }
    return COCO_SUCCESS;
}

int TcpServer::Session::DoCycle() {
    int ret = COCO_SUCCESS;
    const TcpServerOptions &opt = server_->options_;

    SslServer *ssl = nullptr;
    if (!opt.tls_key_file.empty() && !opt.tls_crt_file.empty()) {
        StreamConn *tcp = conn_.release();
        ssl = new SslServer(tcp->GetStfd(), tcp);
        conn_.reset(ssl);
    }

    // SslServer opens its own socket over the fd, so timeouts are set after wrapping.
    conn_->SetRecvTimeout(opt.recv_timeout_us);
    conn_->SetSendTimeout(opt.send_timeout_us);

    if (ssl && (ret = ssl->Handshake(opt.tls_key_file, opt.tls_crt_file)) != COCO_SUCCESS) {
        coco_error("tls handshake failed. ret=%d", ret);
        return ret;
    }

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
    if (listener_) {
        delete listener_;
        listener_ = nullptr;
    }
}

int TcpServer::ListenAndServe(const std::string &ip, int port) {
    TcpListener *l = ListenTcp(ip, port);
    if (l == nullptr) {
        coco_error("tcp server listen failed. ep=%s:%d", ip.c_str(), port);
        return ERROR_SOCKET_LISTEN;
    }
    return Serve(l);
}

int TcpServer::Serve(TcpListener *l) {
    if (acceptor_ != nullptr) {
        delete l;
        coco_error("tcp server already serving");
        return ERROR_THREAD_STARTED;
    }

    listener_ = l;
    acceptor_ = new Acceptor(this);
    return acceptor_->Start();
}

void TcpServer::Stop() {
    // The acceptor must be gone first, or it could start a connection after the shutdown.
    if (acceptor_) {
        acceptor_->Stop();
    }
    manager_.Shutdown();
}
