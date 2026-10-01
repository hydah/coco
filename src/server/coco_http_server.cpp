#include "server/coco_http_server.hpp"

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "net/layer4/coco_tcp.hpp"

HttpServer::HttpServer(HttpHandler *handler, HttpServeOptions options)
    : handler_(handler), options_(options) {}

HttpServer::HttpServer(HttpHandlerFunc handler, HttpServeOptions options)
    : owned_(std::make_shared<HttpFuncHandler>(std::move(handler))),
      handler_(owned_.get()),
      options_(options) {}

HttpServer::~HttpServer() { server_.reset(); }

int HttpServer::ListenAndServe(const std::string &ip, int port) {
    std::unique_ptr<TcpListener> l;
    int ret = ListenTcp(ip, port, &l);
    if (ret != COCO_SUCCESS) {
        coco_error("http: listen on %s:%d failed. ret=%d", ip.c_str(), port, ret);
        return ret;
    }
    return Start(std::move(l), "", "");
}

int HttpServer::ListenAndServeTLS(const std::string &ip, int port, const std::string &crt_file,
                                  const std::string &key_file) {
    std::unique_ptr<TcpListener> l;
    int ret = ListenTcp(ip, port, &l);
    if (ret != COCO_SUCCESS) {
        coco_error("https: listen on %s:%d failed. ret=%d", ip.c_str(), port, ret);
        return ret;
    }
    return Start(std::move(l), crt_file, key_file);
}

int HttpServer::Serve(std::unique_ptr<StreamListener> l) { return Start(std::move(l), "", ""); }

int HttpServer::Start(std::unique_ptr<StreamListener> l, const std::string &crt_file,
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

    HttpHandler *handler = handler_;
    HttpServeOptions options = options_;
    server_.reset(new TcpServer(
        [handler, options](StreamConn &conn) { return ServeHttpConn(conn, handler, options); },
        opt));
    return server_->Serve(std::move(l));
}

void HttpServer::Stop() {
    if (server_) {
        server_->Stop();
    }
}
