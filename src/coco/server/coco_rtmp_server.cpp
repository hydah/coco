#include "coco/server/coco_rtmp_server.hpp"

#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/log/log.hpp"
#include "coco/net/layer4/coco_tcp.hpp"

namespace coco {

RtmpServer::RtmpServer(RtmpHandler handler) : handler_(handler) {}

RtmpServer::~RtmpServer() { server_.reset(); }

int RtmpServer::ListenAndServe(const std::string& ip, int port) {
    int ret = Start(ip, port);
    if (ret == COCO_SUCCESS) {
        Wait();
    }
    return ret;
}

int RtmpServer::ListenAndServeTLS(const std::string& ip, int port, const std::string& crt_file,
                                  const std::string& key_file) {
    int ret = StartTLS(ip, port, crt_file, key_file);
    if (ret == COCO_SUCCESS) {
        Wait();
    }
    return ret;
}

int RtmpServer::Serve(std::unique_ptr<StreamListener> l) {
    int ret = Start(std::move(l));
    if (ret == COCO_SUCCESS) {
        Wait();
    }
    return ret;
}

int RtmpServer::Start(const std::string& ip, int port) {
    std::unique_ptr<TcpListener> l;
    int ret = ListenTcp(ip, port, &l);
    if (ret != COCO_SUCCESS) {
        coco_error("rtmp: listen on %s:%d failed. ret=%d", ip.c_str(), port, ret);
        return ret;
    }
    return StartOn(std::move(l), "", "");
}

int RtmpServer::StartTLS(const std::string& ip, int port, const std::string& crt_file,
                         const std::string& key_file) {
    std::unique_ptr<TcpListener> l;
    int ret = ListenTcp(ip, port, &l);
    if (ret != COCO_SUCCESS) {
        coco_error("rtmps: listen on %s:%d failed. ret=%d", ip.c_str(), port, ret);
        return ret;
    }
    return StartOn(std::move(l), crt_file, key_file);
}

int RtmpServer::Start(std::unique_ptr<StreamListener> l) { return StartOn(std::move(l), "", ""); }

void RtmpServer::Wait() {
    if (server_) {
        server_->Wait();
    }
}

int RtmpServer::StartOn(std::unique_ptr<StreamListener> l, const std::string& crt_file,
                        const std::string& key_file) {
    if (server_ != nullptr) {
        coco_error("rtmp server already serving");
        return ERROR_THREAD_STARTED;
    }
    TcpServerOptions opt;
    opt.tls_crt_file = crt_file;
    opt.tls_key_file = key_file;
    RtmpHandler handler = handler_;
    server_.reset(
        new TcpServer([handler](StreamConn& conn) { return ServeRtmpConn(conn, handler); }, opt));
    return server_->Start(std::move(l));
}

void RtmpServer::Stop() {
    if (server_) {
        server_->Stop();
    }
}

}  // namespace coco
