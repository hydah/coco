#include "coco/server/coco_http_server.hpp"

#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/log/log.hpp"
#include "coco/net/layer4/coco_tcp.hpp"

namespace coco {

HttpServer::HttpServer(HttpHandler *handler, HttpServeOptions options)
    : handler_(handler), options_(options) {}

HttpServer::HttpServer(HttpHandlerFunc handler, HttpServeOptions options)
    : owned_(std::make_shared<HttpFuncHandler>(std::move(handler))),
      handler_(owned_.get()),
      options_(options) {}

HttpServer::~HttpServer() { server_.reset(); }

int HttpServer::ListenAndServe(const std::string &ip, int port) {
    int ret = Start(ip, port);
    if (ret == COCO_SUCCESS) {
        Wait();
    }
    return ret;
}

int HttpServer::ListenAndServeTLS(const std::string &ip, int port, const std::string &crt_file,
                                  const std::string &key_file) {
    int ret = StartTLS(ip, port, crt_file, key_file);
    if (ret == COCO_SUCCESS) {
        Wait();
    }
    return ret;
}

int HttpServer::Serve(std::unique_ptr<StreamListener> l) {
    int ret = Start(std::move(l));
    if (ret == COCO_SUCCESS) {
        Wait();
    }
    return ret;
}

int HttpServer::Start(const std::string &ip, int port) {
    std::unique_ptr<TcpListener> l;
    int ret = ListenTcp(ip, port, &l);
    if (ret != COCO_SUCCESS) {
        coco_error("http: listen on %s:%d failed. ret=%d", ip.c_str(), port, ret);
        return ret;
    }
    return StartOn(std::move(l), "", "");
}

int HttpServer::StartTLS(const std::string &ip, int port, const std::string &crt_file,
                         const std::string &key_file) {
    std::unique_ptr<TcpListener> l;
    int ret = ListenTcp(ip, port, &l);
    if (ret != COCO_SUCCESS) {
        coco_error("https: listen on %s:%d failed. ret=%d", ip.c_str(), port, ret);
        return ret;
    }
    return StartOn(std::move(l), crt_file, key_file);
}

int HttpServer::Start(std::unique_ptr<StreamListener> l) { return StartOn(std::move(l), "", ""); }

void HttpServer::Wait() {
    if (server_) {
        server_->Wait();
    }
}

int HttpServer::StartOn(std::unique_ptr<StreamListener> l, const std::string &crt_file,
                        const std::string &key_file) {
    if (server_ != nullptr) {
        coco_error("http server already serving");
        return ERROR_THREAD_STARTED;
    }

    TcpServerOptions opt;
    // They also bound the TLS handshake, which runs before ServeHttpConn sets its own.
    opt.recv_timeout_us = options_.read_timeout_us;
    opt.send_timeout_us = options_.write_timeout_us;
    opt.tls_crt_file = crt_file;
    opt.tls_key_file = key_file;
    opt.threads = options_.threads;

    HttpHandler *handler = handler_;
    HttpServeOptions options = options_;
    server_.reset(new TcpServer(
        [handler, options](StreamConn &conn) { return ServeHttpConn(conn, handler, options); },
        opt));
    return server_->Start(std::move(l));
}

void HttpServer::Stop() {
    if (server_) {
        server_->Stop();
    }
}

}  // namespace coco
