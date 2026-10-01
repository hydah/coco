#pragma once

#include <memory>
#include <string>

#include "coco/net/layer7/rtmp/coco_rtmp.hpp"
#include "coco/server/coco_tcp_server.hpp"

namespace coco {

// An RTMP server: a TcpServer whose handler is ServeRtmpConn.
//
//   RtmpServer server([](RtmpConn &conn, const RtmpRequest &req) {
//       RtmpMessage msg;
//       while (conn.ReadMessage(&msg) == COCO_SUCCESS) {
//       }
//       return COCO_SUCCESS;
//   });
//   return server.ListenAndServe("0.0.0.0", 1935);
//
// RTMPS is RTMP over TLS: ListenAndServeTLS, or pass a TlsListener to Serve.
class RtmpServer {
 public:
    explicit RtmpServer(RtmpHandler handler);
    virtual ~RtmpServer();

    int ListenAndServe(const std::string& ip, int port);
    int ListenAndServeTLS(const std::string& ip, int port, const std::string& crt_file,
                          const std::string& key_file);
    int Serve(std::unique_ptr<StreamListener> l);

    int Start(const std::string& ip, int port);
    int StartTLS(const std::string& ip, int port, const std::string& crt_file,
                 const std::string& key_file);
    int Start(std::unique_ptr<StreamListener> l);
    void Wait();
    void Stop();

 private:
    int StartOn(std::unique_ptr<StreamListener> l, const std::string& crt_file,
                const std::string& key_file);

    RtmpHandler handler_;
    std::unique_ptr<TcpServer> server_;
};

}  // namespace coco
