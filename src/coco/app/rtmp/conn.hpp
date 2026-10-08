#pragma once

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "coco/net/conn.hpp"
#include "coco/app/rtmp/codec/amf0.hpp"
#include "coco/app/rtmp/codec/chunk.hpp"

namespace coco {

constexpr uint32_t kRtmpAckWindow = 2500000;
constexpr uint32_t kRtmpOutChunkSize = 4096;

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

// Writes values as one AMF0 command message on stream_id.
int RtmpWriteCommand(RtmpConn* conn, uint32_t stream_id, const std::vector<Amf0Value>& values);

}  // namespace coco
