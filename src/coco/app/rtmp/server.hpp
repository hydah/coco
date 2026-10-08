#pragma once

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>

#include "coco/net/conn.hpp"
#include "coco/net/tcp_server.hpp"
#include "coco/app/rtmp/conn.hpp"

namespace coco {

// What the peer asked for, after connect and createStream have been answered.
struct RtmpRequest {
    std::string app;
    std::string stream;
    std::string tc_url;
    std::string flash_ver;
    std::string publish_type;
    bool publish = false;
    uint32_t stream_id = 0;
};

// Runs for the rest of the connection. publish is true when the peer is sending media;
// otherwise it expects media to be written. Returning closes the connection.
typedef std::function<int(RtmpConn& conn, const RtmpRequest& req)> RtmpHandler;

// Handshake, connect, createStream, then publish or play. The success status
// (NetStream.Publish.Start, or Play.Reset and Play.Start) is sent before handler runs.
int ServeRtmpConn(StreamConn& conn, const RtmpHandler& handler);

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
