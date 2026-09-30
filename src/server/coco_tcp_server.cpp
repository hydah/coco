#include "server/coco_tcp_server.hpp"

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "net/layer4/coco_tcp.hpp"
#include "net/tls/coco_tls.hpp"

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
    std::unique_ptr<TcpListener> l;
    int ret = ListenTcp(ip, port, &l);
    if (ret != COCO_SUCCESS) {
        coco_error("tcp server listen failed. ep=%s:%d, ret=%d", ip.c_str(), port, ret);
        return ret;
    }
    return Serve(std::move(l));
}

int TcpServer::Serve(std::unique_ptr<StreamListener> l) {
    int ret = COCO_SUCCESS;

    if (acceptor_ != nullptr) {
        coco_error("tcp server already serving");
        return ERROR_THREAD_STARTED;
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

void TcpServer::Stop() {
    // The acceptor must be gone first, or it could start a connection after the shutdown.
    if (acceptor_) {
        acceptor_->Stop();
    }
    manager_.Shutdown();
}
