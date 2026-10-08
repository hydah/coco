#include "coco/net/rudp/codec/packet.hpp"

namespace coco {

namespace {

void PutU16(std::string *out, uint16_t v) {
    out->push_back((char)(v >> 8));
    out->push_back((char)(v & 0xff));
}

void PutU32(std::string *out, uint32_t v) {
    PutU16(out, (uint16_t)(v >> 16));
    PutU16(out, (uint16_t)(v & 0xffff));
}

uint16_t GetU16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

uint32_t GetU32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

}  // namespace

void EncodeRudpPacket(const RudpPacket &p, std::string *out) {
    out->reserve(out->size() + kRudpHeaderSize + p.payload.size());
    out->push_back((char)kRudpVersion);
    out->push_back((char)p.type);
    PutU16(out, p.window);
    PutU32(out, p.conn_id);
    PutU32(out, p.seq);
    PutU32(out, p.ack);
    out->append(p.payload);
}

bool DecodeRudpPacket(const uint8_t *data, size_t size, RudpPacket *p) {
    if (size < kRudpHeaderSize || size > kRudpMaxDatagram || data[0] != kRudpVersion) {
        return false;
    }
    uint8_t type = data[1];
    if (type < (uint8_t)RudpType::kSyn || type > (uint8_t)RudpType::kRst) {
        return false;
    }
    size_t payload = size - kRudpHeaderSize;
    if ((RudpType)type == RudpType::kData ? payload == 0 : payload != 0) {
        return false;
    }
    p->type = (RudpType)type;
    p->window = GetU16(data + 2);
    p->conn_id = GetU32(data + 4);
    p->seq = GetU32(data + 8);
    p->ack = GetU32(data + 12);
    if (p->conn_id == 0) {
        return false;
    }
    p->payload.assign((const char *)data + kRudpHeaderSize, payload);
    return true;
}

}  // namespace coco
