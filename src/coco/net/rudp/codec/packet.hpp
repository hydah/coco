#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace coco {

// One RUDP packet per UDP datagram: a 16-byte header in network byte order, then the
// payload of a DATA packet. The payload length is the datagram length minus the header.
//
//   version(8) type(8) window(16) | conn_id(32) | seq(32) | ack(32) | payload
constexpr uint8_t kRudpVersion = 1;
constexpr size_t kRudpHeaderSize = 16;
// Payload of one DATA packet at most; a datagram stays below the IPv6 minimum MTU.
constexpr size_t kRudpMss = 1200;
constexpr size_t kRudpMaxDatagram = kRudpHeaderSize + kRudpMss;

enum class RudpType : uint8_t {
    kSyn = 1,
    kSynAck = 2,
    kAck = 3,
    kData = 4,
    kFin = 5,
    kRst = 6,
};

struct RudpPacket {
    RudpType type = RudpType::kAck;
    // Receive window of the sender, in segments.
    uint16_t window = 0;
    uint32_t conn_id = 0;
    uint32_t seq = 0;
    // The sender's next expected sequence number: everything before it arrived in order.
    uint32_t ack = 0;
    std::string payload;
};

// True when a comes before b in 32-bit serial number arithmetic (RFC 1982).
inline bool RudpBefore(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

// Appends the datagram for p to *out.
void EncodeRudpPacket(const RudpPacket &p, std::string *out);

// Parses one datagram. Fails, leaving *p unspecified, unless it is 16 to kRudpMaxDatagram
// bytes of version 1 with a known type and a non-zero conn_id, and only a DATA packet
// carries a payload, of 1 to kRudpMss bytes.
bool DecodeRudpPacket(const uint8_t *data, size_t size, RudpPacket *p);

}  // namespace coco
