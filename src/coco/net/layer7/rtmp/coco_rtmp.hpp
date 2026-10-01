#pragma once

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "coco/net/layer4/coco_layer4.hpp"
#include "coco/net/layer7/rtmp/rtmp_amf0.hpp"
#include "coco/net/layer7/rtmp/rtmp_chunk.hpp"

namespace coco {

constexpr uint32_t kRtmpDefaultPort = 1935;
constexpr uint32_t kRtmpAckWindow = 2500000;
constexpr uint32_t kRtmpOutChunkSize = 4096;

// rtmp://host[:port]/app[/stream] or rtmps://. app is the first path segment, stream is
// the rest. tc_url is rtmp://host[:port]/app, with the port omitted when it is 1935.
struct RtmpUrl {
    bool tls = false;
    std::string host;
    int port = kRtmpDefaultPort;
    std::string app;
    std::string stream;
    std::string tc_url;
};

int ParseRtmpUrl(const std::string& url, RtmpUrl* out);

// One RTMP connection after the handshake. ReadMessage returns command, data, audio and
// video messages. Chunk size, acknowledgements, peer bandwidth and ping are handled
// here, so a publish handler just reads and a play handler must still be read from
// eventually if the peer's control messages should not sit in the socket buffer.
// Writes are serialized: ReadMessage's replies and WriteMessage do not interleave.
class RtmpConn {
 public:
    // conn is not owned and must outlive this object.
    explicit RtmpConn(StreamConn* conn);
    ~RtmpConn();

    RtmpConn(const RtmpConn&) = delete;
    RtmpConn& operator=(const RtmpConn&) = delete;

    int ReadMessage(RtmpMessage* msg);
    // csid 0 selects one from the message type. stream_id and timestamp are the caller's.
    int WriteMessage(const RtmpMessage& msg);

    int SetChunkSize(uint32_t size);
    int WriteWindowAckSize(uint32_t size);
    // limit_type: 0 hard, 1 soft, 2 dynamic.
    int WritePeerBandwidth(uint32_t size, uint8_t limit_type);

    std::string RemoteAddr() const { return conn_->RemoteAddr(); }

 private:
    struct State;

    int WriteControl(uint8_t type, const void* data, size_t n);
    int OnProtocol(const RtmpMessage& msg, bool* handled);
    int MaybeAck();

    StreamConn* conn_;
    std::unique_ptr<State> st_;
};

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
// rtmps:// needs SetDialer(TlsDialer()) from net/tls first.
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
