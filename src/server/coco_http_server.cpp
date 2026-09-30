#include "server/coco_http_server.hpp"

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"

HttpServer::HttpServer(bool https) : https_(https) {}

HttpServer::~HttpServer() {
    if (server_) {
        delete server_;
        server_ = nullptr;
    }
}

TcpServerOptions HttpServer::Options() const {
    TcpServerOptions opt;
    // Also bounds the TLS handshake, which runs before ServeHttpConn sets its own timeout.
    opt.recv_timeout_us = HTTP_RECV_TIMEOUT_US;
    if (https_) {
        opt.tls_key_file = "./server.key";
        opt.tls_crt_file = "./server.crt";
    }
    return opt;
}

int HttpServer::ListenAndServe(std::string local_ip, int local_port, HttpServeMux *mux) {
    TcpListener *l = ListenTcp(local_ip, local_port);
    if (l == nullptr) {
        coco_error("create http listen socket failed");
        return ERROR_SOCKET_LISTEN;
    }
    return Serve(l, mux);
}

int HttpServer::Serve(TcpListener *l, HttpServeMux *mux) {
    if (server_ != nullptr) {
        delete l;
        coco_error("http server already serving");
        return ERROR_THREAD_STARTED;
    }

    server_ = new TcpServer([mux](StreamConn &conn) { return ServeHttpConn(conn, mux); },
                            Options());
    return server_->Serve(l);
}

void HttpServer::Stop() {
    if (server_) {
        server_->Stop();
    }
}
