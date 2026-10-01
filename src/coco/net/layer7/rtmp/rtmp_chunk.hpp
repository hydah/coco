#pragma once

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>

#include "coco/utils/io.hpp"

namespace coco {

// One reassembled RTMP message. The length field is 24 bits, so a message cannot be
// larger than this.
constexpr size_t kRtmpMaxMessage = 0xFFFFFF;

enum {
    RTMP_MSG_SET_CHUNK_SIZE = 1,
    RTMP_MSG_ABORT = 2,
    RTMP_MSG_ACK = 3,
    RTMP_MSG_USER_CONTROL = 4,
    RTMP_MSG_WINDOW_ACK_SIZE = 5,
    RTMP_MSG_SET_PEER_BANDWIDTH = 6,
    RTMP_MSG_AUDIO = 8,
    RTMP_MSG_VIDEO = 9,
    RTMP_MSG_DATA_AMF3 = 15,
    RTMP_MSG_SHARED_OBJECT_AMF3 = 16,
    RTMP_MSG_COMMAND_AMF3 = 17,
    RTMP_MSG_DATA_AMF0 = 18,
    RTMP_MSG_SHARED_OBJECT_AMF0 = 19,
    RTMP_MSG_COMMAND_AMF0 = 20,
    RTMP_MSG_AGGREGATE = 22,
};

struct RtmpMessage {
    uint32_t timestamp = 0;
    uint32_t stream_id = 0;
    uint32_t csid = 0;
    uint8_t type = 0;
    std::string payload;
};

// Reassembles chunk streams. fmt 3 repeats the extended timestamp whenever the previous
// header on that chunk stream used 0xFFFFFF, which is what FFmpeg writes and reads
// (libavformat/rtmppkt.c). A new fmt 3 message adds that value; a continuation chunk
// only consumes it.
class RtmpChunkReader {
 public:
    explicit RtmpChunkReader(IoReader* in) : in_(in) {}

    // The next complete message, whichever chunk stream finishes first. *wire_bytes is
    // how many bytes this call read, including headers of messages still incomplete.
    int ReadMessage(RtmpMessage* msg, size_t* wire_bytes);
    void SetChunkSize(uint32_t size);
    uint32_t ChunkSize() const { return chunk_size_; }
    void Abort(uint32_t csid);

 private:
    struct Cs {
        bool seen = false;
        bool open = false;
        uint32_t timestamp = 0;
        uint32_t last_ts_field = 0;
        uint32_t msg_len = 0;
        uint8_t type = 0;
        uint32_t stream_id = 0;
        uint32_t got = 0;
        std::string payload;
    };

    int ReadFull(void* buf, size_t n, size_t* wire);
    int ReadChunk(size_t* wire);

    IoReader* in_;
    uint32_t chunk_size_ = 128;
    std::map<uint32_t, Cs> streams_;
    // Set when ReadChunk finishes a message; ReadMessage returns it.
    uint32_t done_csid_ = 0;
    bool done_ = false;
};

// Writes one message as chunks. The first chunk of a message is fmt 0, 1 or 2; further
// chunks of the same message are fmt 3. csid is not taken from msg so a caller can pick
// one without copying the payload.
class RtmpChunkWriter {
 public:
    int Encode(uint32_t csid, const RtmpMessage& msg, std::string* out);
    void SetChunkSize(uint32_t size);
    uint32_t ChunkSize() const { return chunk_size_; }

 private:
    struct Os {
        bool seen = false;
        uint32_t timestamp = 0;
        uint32_t msg_len = 0;
        uint8_t type = 0;
        uint32_t stream_id = 0;
    };

    uint32_t chunk_size_ = 128;
    std::map<uint32_t, Os> streams_;
};

}  // namespace coco
