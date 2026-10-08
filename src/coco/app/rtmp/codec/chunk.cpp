#include "coco/app/rtmp/codec/chunk.hpp"

#include "coco/common/error.hpp"
#include "coco/app/rtmp/codec/bytes.hpp"

namespace coco {

namespace {

void PutBasic(std::string* o, uint8_t fmt, uint32_t csid) {
    if (csid < 64) {
        o->push_back((char)((fmt << 6) | csid));
    } else if (csid < 320) {
        o->push_back((char)(fmt << 6));
        o->push_back((char)(csid - 64));
    } else {
        uint32_t v = csid - 64;
        o->push_back((char)((fmt << 6) | 1));
        o->push_back((char)v);
        o->push_back((char)(v >> 8));
    }
}

}  // namespace

int RtmpChunkReader::ReadFull(void* buf, size_t n, size_t* wire) {
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t k = 0;
        int ret = in_->Read(p + got, n - got, &k);
        if (ret != COCO_SUCCESS) {
            return ret;
        }
        if (k <= 0) {
            return ERROR_SOCKET_READ;
        }
        got += (size_t)k;
    }
    *wire += n;
    return COCO_SUCCESS;
}

void RtmpChunkReader::SetChunkSize(uint32_t size) {
    if (size >= 1 && size <= 0x7FFFFFFF) {
        chunk_size_ = size;
    }
}

void RtmpChunkReader::Abort(uint32_t csid) {
    std::map<uint32_t, Cs>::iterator it = streams_.find(csid);
    if (it == streams_.end()) {
        return;
    }
    it->second.open = false;
    it->second.got = 0;
    it->second.payload.clear();
}

int RtmpChunkReader::ReadChunk(size_t* wire) {
    uint8_t b0 = 0;
    int ret = ReadFull(&b0, 1, wire);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    uint8_t fmt = b0 >> 6;
    uint32_t csid = b0 & 0x3f;
    if (csid == 0) {
        uint8_t b = 0;
        if ((ret = ReadFull(&b, 1, wire)) != COCO_SUCCESS) {
            return ret;
        }
        csid = (uint32_t)b + 64;
    } else if (csid == 1) {
        uint8_t b[2];
        if ((ret = ReadFull(b, 2, wire)) != COCO_SUCCESS) {
            return ret;
        }
        csid = (uint32_t)b[0] | ((uint32_t)b[1] << 8);
        csid += 64;
    }
    if (csid < 2) {
        return ERROR_RTMP_PROTOCOL;
    }
    Cs& cs = streams_[csid];
    done_ = false;

    if (fmt == 3) {
        if (!cs.seen) {
            return ERROR_RTMP_PROTOCOL;
        }
        uint32_t add = cs.last_ts_field;
        if (cs.last_ts_field == 0xFFFFFF) {
            uint8_t e[4];
            if ((ret = ReadFull(e, 4, wire)) != COCO_SUCCESS) {
                return ret;
            }
            add = RtmpBe32(e);
        }
        if (!cs.open) {
            if (cs.msg_len > kRtmpMaxMessage) {
                return ERROR_RTMP_MESSAGE_TOO_LARGE;
            }
            cs.timestamp += add;
            cs.got = 0;
            cs.payload.clear();
            cs.open = true;
        }
    } else {
        if (!cs.seen && fmt != 0) {
            return ERROR_RTMP_PROTOCOL;
        }
        if (cs.open && cs.got < cs.msg_len) {
            return ERROR_RTMP_PROTOCOL;
        }
        uint8_t ts3[3];
        if ((ret = ReadFull(ts3, 3, wire)) != COCO_SUCCESS) {
            return ret;
        }
        uint32_t ts_field = RtmpBe24(ts3);
        if (fmt <= 1) {
            uint8_t lenb[3];
            uint8_t type = 0;
            if ((ret = ReadFull(lenb, 3, wire)) != COCO_SUCCESS) {
                return ret;
            }
            if ((ret = ReadFull(&type, 1, wire)) != COCO_SUCCESS) {
                return ret;
            }
            cs.msg_len = RtmpBe24(lenb);
            cs.type = type;
            if (fmt == 0) {
                uint8_t sid[4];
                if ((ret = ReadFull(sid, 4, wire)) != COCO_SUCCESS) {
                    return ret;
                }
                cs.stream_id = RtmpLe32(sid);
            }
        }
        uint32_t ts = ts_field;
        if (ts_field == 0xFFFFFF) {
            uint8_t e[4];
            if ((ret = ReadFull(e, 4, wire)) != COCO_SUCCESS) {
                return ret;
            }
            ts = RtmpBe32(e);
        }
        if (cs.msg_len > kRtmpMaxMessage) {
            return ERROR_RTMP_MESSAGE_TOO_LARGE;
        }
        cs.last_ts_field = ts_field;
        cs.seen = true;
        if (fmt == 0) {
            cs.timestamp = ts;
        } else {
            cs.timestamp += ts;
        }
        cs.got = 0;
        cs.payload.clear();
        cs.open = true;
    }

    if (chunk_size_ == 0) {
        return ERROR_RTMP_PROTOCOL;
    }
    uint32_t remain = cs.msg_len - cs.got;
    uint32_t n = remain < chunk_size_ ? remain : chunk_size_;
    if (n > 0) {
        size_t at = cs.payload.size();
        cs.payload.resize(at + n);
        if ((ret = ReadFull(&cs.payload[at], n, wire)) != COCO_SUCCESS) {
            return ret;
        }
        cs.got += n;
    }
    if (cs.got == cs.msg_len) {
        cs.open = false;
        done_ = true;
        done_csid_ = csid;
    }
    return COCO_SUCCESS;
}

int RtmpChunkReader::ReadMessage(RtmpMessage* msg, size_t* wire_bytes) {
    size_t wire = 0;
    while (true) {
        int ret = ReadChunk(&wire);
        if (ret != COCO_SUCCESS) {
            return ret;
        }
        if (!done_) {
            continue;
        }
        Cs& cs = streams_[done_csid_];
        msg->timestamp = cs.timestamp;
        msg->stream_id = cs.stream_id;
        msg->type = cs.type;
        msg->csid = done_csid_;
        msg->payload.swap(cs.payload);
        cs.payload.clear();
        if (wire_bytes) {
            *wire_bytes = wire;
        }
        return COCO_SUCCESS;
    }
}

void RtmpChunkWriter::SetChunkSize(uint32_t size) {
    if (size >= 1 && size <= 0x7FFFFFFF) {
        chunk_size_ = size;
    }
}

int RtmpChunkWriter::Encode(uint32_t csid, const RtmpMessage& msg, std::string* out) {
    if (csid < 2 || csid > 65599 || chunk_size_ == 0) {
        return ERROR_RTMP_PROTOCOL;
    }
    if (msg.payload.size() > kRtmpMaxMessage) {
        return ERROR_RTMP_MESSAGE_TOO_LARGE;
    }
    Os& st = streams_[csid];
    uint8_t fmt = 0;
    uint32_t full = msg.timestamp;
    if (st.seen && msg.stream_id == st.stream_id && msg.timestamp >= st.timestamp) {
        full = msg.timestamp - st.timestamp;
        bool same = st.type == msg.type && st.msg_len == msg.payload.size();
        fmt = same ? 2 : 1;
    }
    uint32_t marker = full >= 0xFFFFFF ? 0xFFFFFF : full;

    size_t off = 0;
    do {
        uint8_t use = off == 0 ? fmt : 3;
        PutBasic(out, use, csid);
        if (use != 3) {
            RtmpPutBe24(out, marker);
            if (use != 2) {
                RtmpPutBe24(out, (uint32_t)msg.payload.size());
                out->push_back((char)msg.type);
                if (use == 0) {
                    RtmpPutLe32(out, msg.stream_id);
                }
            }
        }
        if (marker == 0xFFFFFF) {
            RtmpPutBe32(out, full);
        }
        size_t remain = msg.payload.size() - off;
        size_t n = remain < chunk_size_ ? remain : chunk_size_;
        if (n > 0) {
            out->append(msg.payload, off, n);
            off += n;
        }
    } while (off < msg.payload.size());

    st.seen = true;
    st.timestamp = msg.timestamp;
    st.msg_len = (uint32_t)msg.payload.size();
    st.type = msg.type;
    st.stream_id = msg.stream_id;
    return COCO_SUCCESS;
}

uint32_t RtmpDefaultCsid(const RtmpMessage& msg) {
    switch (msg.type) {
        case RTMP_MSG_SET_CHUNK_SIZE:
        case RTMP_MSG_ABORT:
        case RTMP_MSG_ACK:
        case RTMP_MSG_USER_CONTROL:
        case RTMP_MSG_WINDOW_ACK_SIZE:
        case RTMP_MSG_SET_PEER_BANDWIDTH:
            return 2;
        case RTMP_MSG_AUDIO:
            return 6;
        case RTMP_MSG_VIDEO:
            return 7;
        case RTMP_MSG_DATA_AMF0:
        case RTMP_MSG_DATA_AMF3:
            return 5;
        case RTMP_MSG_COMMAND_AMF0:
        case RTMP_MSG_COMMAND_AMF3:
            return msg.stream_id == 0 ? 3 : 4;
        default:
            return 3;
    }
}

}  // namespace coco
