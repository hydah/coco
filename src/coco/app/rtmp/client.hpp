#pragma once

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "coco/net/conn.hpp"
#include "coco/app/rtmp/codec/url.hpp"
#include "coco/app/rtmp/conn.hpp"

namespace coco {

// A publishing or playing client.
//
//   RtmpClient pub;
//   pub.Dial("rtmp://127.0.0.1:1935/live/stream");
//   pub.Publish();
//   RtmpMessage msg;
//   msg.type = RTMP_MSG_AUDIO;
//   msg.payload = ...;
//   pub.WriteMessage(msg);
//
// rtmps:// needs SetDialer(TlsDialer()) from coco/net/tls first.
class RtmpClient {
 public:
    RtmpClient();
    ~RtmpClient();

    RtmpClient(const RtmpClient&) = delete;
    RtmpClient& operator=(const RtmpClient&) = delete;

    // Default is TcpDialer(). rtmps:// fails with ERROR_RTMP_URL until this is set.
    void SetDialer(StreamDialer dialer);

    // Connects and completes the connect command. timeout_us bounds the connect, the
    // handshake and every later read and write.
    int Dial(const std::string& url, int64_t timeout_us = 3 * 1000 * 1000);
    int Publish();
    int Play();

    int ReadMessage(RtmpMessage* msg);
    // A zero stream_id is replaced with the id from Publish or Play.
    int WriteMessage(const RtmpMessage& msg);
    uint32_t StreamId() const { return stream_id_; }
    const RtmpUrl& Url() const { return url_; }

 private:
    int WriteCommand(uint32_t stream_id, const std::vector<Amf0Value>& values);
    int WaitResult(double txid, Amf0Value* info, double* number);
    int WaitStatus(const std::string& code);

    StreamDialer dialer_;
    bool dialer_set_ = false;
    RtmpUrl url_;
    std::unique_ptr<StreamConn> raw_;
    std::unique_ptr<RtmpConn> conn_;
    uint32_t stream_id_ = 0;
    double tx_ = 0;
};

}  // namespace coco
