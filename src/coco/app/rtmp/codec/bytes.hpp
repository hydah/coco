#pragma once

#include <stdint.h>
#include <string.h>

#include <string>

namespace coco {

inline uint16_t RtmpBe16(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }

inline uint32_t RtmpBe24(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

inline uint32_t RtmpBe32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

inline uint32_t RtmpLe32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline void RtmpStoreBe32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

inline void RtmpPutBe16(std::string* o, uint16_t v) {
    o->push_back((char)(v >> 8));
    o->push_back((char)v);
}

inline void RtmpPutBe24(std::string* o, uint32_t v) {
    o->push_back((char)(v >> 16));
    o->push_back((char)(v >> 8));
    o->push_back((char)v);
}

inline void RtmpPutBe32(std::string* o, uint32_t v) {
    o->push_back((char)(v >> 24));
    o->push_back((char)(v >> 16));
    o->push_back((char)(v >> 8));
    o->push_back((char)v);
}

inline void RtmpPutLe32(std::string* o, uint32_t v) {
    o->push_back((char)v);
    o->push_back((char)(v >> 8));
    o->push_back((char)(v >> 16));
    o->push_back((char)(v >> 24));
}

// IEEE 754 binary64, big-endian on the wire. The integer holds the bits, so this
// does not depend on the host endianness.
inline double RtmpBeDouble(const uint8_t* p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) {
        u = (u << 8) | p[i];
    }
    double d;
    memcpy(&d, &u, 8);
    return d;
}

inline void RtmpPutBeDouble(std::string* o, double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    for (int i = 7; i >= 0; i--) {
        o->push_back((char)((u >> (i * 8)) & 0xff));
    }
}

}  // namespace coco
