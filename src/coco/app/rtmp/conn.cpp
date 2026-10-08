#include "coco/app/rtmp/conn.hpp"

#include <string.h>

#include "st.h"

#include "coco/common/error.hpp"
#include "coco/log/log.hpp"
#include "coco/app/rtmp/codec/bytes.hpp"

namespace coco {

struct RtmpConn::State {
    explicit State(StreamConn* conn)
        : reader(conn), write_lock(st_mutex_new()), ack_window(kRtmpAckWindow) {}
    ~State() {
        if (write_lock) {
            st_mutex_destroy(write_lock);
        }
    }

    RtmpChunkReader reader;
    RtmpChunkWriter writer;
    st_mutex_t write_lock = nullptr;
    uint32_t bytes_in = 0;
    uint32_t last_ack = 0;
    uint32_t ack_window = 0;
};

RtmpConn::RtmpConn(StreamConn* conn) : conn_(conn), st_(new State(conn)) {}

RtmpConn::~RtmpConn() {}

int RtmpConn::WriteControl(uint8_t type, const void* data, size_t n) {
    RtmpMessage msg;
    msg.type = type;
    msg.csid = 2;
    if (n) {
        msg.payload.assign((const char*)data, n);
    }
    return WriteMessage(msg);
}

int RtmpConn::SetChunkSize(uint32_t size) {
    if (size < 1 || size > 0x7FFFFFFF) {
        return ERROR_RTMP_PROTOCOL;
    }
    uint8_t b[4];
    RtmpStoreBe32(b, size);
    int ret = WriteControl(RTMP_MSG_SET_CHUNK_SIZE, b, 4);
    if (ret == COCO_SUCCESS) {
        st_->writer.SetChunkSize(size);
    }
    return ret;
}

int RtmpConn::WriteWindowAckSize(uint32_t size) {
    uint8_t b[4];
    RtmpStoreBe32(b, size);
    return WriteControl(RTMP_MSG_WINDOW_ACK_SIZE, b, 4);
}

int RtmpConn::WritePeerBandwidth(uint32_t size, uint8_t limit_type) {
    uint8_t b[5];
    RtmpStoreBe32(b, size);
    b[4] = limit_type;
    return WriteControl(RTMP_MSG_SET_PEER_BANDWIDTH, b, 5);
}

int RtmpConn::MaybeAck() {
    if (st_->ack_window == 0 || st_->bytes_in - st_->last_ack < st_->ack_window) {
        return COCO_SUCCESS;
    }
    uint8_t b[4];
    RtmpStoreBe32(b, st_->bytes_in);
    int ret = WriteControl(RTMP_MSG_ACK, b, 4);
    if (ret == COCO_SUCCESS) {
        st_->last_ack = st_->bytes_in;
    }
    return ret;
}

int RtmpConn::OnProtocol(const RtmpMessage& msg, bool* handled) {
    *handled = true;
    const uint8_t* p = (const uint8_t*)msg.payload.data();
    size_t n = msg.payload.size();
    switch (msg.type) {
        case RTMP_MSG_SET_CHUNK_SIZE: {
            if (n < 4) {
                return ERROR_RTMP_PROTOCOL;
            }
            uint32_t size = RtmpBe32(p) & 0x7FFFFFFF;
            if (size < 1) {
                return ERROR_RTMP_PROTOCOL;
            }
            st_->reader.SetChunkSize(size);
            return COCO_SUCCESS;
        }
        case RTMP_MSG_ABORT:
            if (n < 4) {
                return ERROR_RTMP_PROTOCOL;
            }
            st_->reader.Abort(RtmpBe32(p));
            return COCO_SUCCESS;
        case RTMP_MSG_ACK:
            return COCO_SUCCESS;
        case RTMP_MSG_USER_CONTROL: {
            if (n < 2) {
                return ERROR_RTMP_PROTOCOL;
            }
            uint16_t ev = RtmpBe16(p);
            if (ev == 6 && n >= 6) {
                uint8_t b[6];
                b[0] = 0;
                b[1] = 7;
                memcpy(b + 2, p + 2, 4);
                return WriteControl(RTMP_MSG_USER_CONTROL, b, 6);
            }
            return COCO_SUCCESS;
        }
        case RTMP_MSG_WINDOW_ACK_SIZE:
            if (n < 4) {
                return ERROR_RTMP_PROTOCOL;
            }
            st_->ack_window = RtmpBe32(p);
            return COCO_SUCCESS;
        case RTMP_MSG_SET_PEER_BANDWIDTH: {
            if (n < 4) {
                return ERROR_RTMP_PROTOCOL;
            }
            uint32_t size = RtmpBe32(p);
            st_->ack_window = size;
            uint8_t b[4];
            RtmpStoreBe32(b, size);
            return WriteControl(RTMP_MSG_WINDOW_ACK_SIZE, b, 4);
        }
        default:
            *handled = false;
            return COCO_SUCCESS;
    }
}

int RtmpConn::ReadMessage(RtmpMessage* msg) {
    while (true) {
        size_t wire = 0;
        int ret = st_->reader.ReadMessage(msg, &wire);
        if (ret != COCO_SUCCESS) {
            return ret;
        }
        st_->bytes_in += (uint32_t)wire;
        if ((ret = MaybeAck()) != COCO_SUCCESS) {
            return ret;
        }
        bool handled = false;
        if ((ret = OnProtocol(*msg, &handled)) != COCO_SUCCESS) {
            return ret;
        }
        if (!handled) {
            return COCO_SUCCESS;
        }
    }
}

int RtmpConn::WriteMessage(const RtmpMessage& msg) {
    if (!st_->write_lock || st_mutex_lock(st_->write_lock) != 0) {
        return ERROR_RTMP_PROTOCOL;
    }
    uint32_t csid = msg.csid ? msg.csid : RtmpDefaultCsid(msg);
    std::string bytes;
    int ret = st_->writer.Encode(csid, msg, &bytes);
    if (ret == COCO_SUCCESS && !bytes.empty()) {
        ssize_t n = 0;
        ret = conn_->Write((void*)bytes.data(), bytes.size(), &n);
    }
    st_mutex_unlock(st_->write_lock);
    return ret;
}

int RtmpWriteCommand(RtmpConn* conn, uint32_t stream_id, const std::vector<Amf0Value>& values) {
    RtmpMessage msg;
    msg.type = RTMP_MSG_COMMAND_AMF0;
    msg.stream_id = stream_id;
    msg.payload = Amf0Encode(values);
    return conn->WriteMessage(msg);
}

}  // namespace coco
