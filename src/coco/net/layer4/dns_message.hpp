#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace coco {

// RFC 1035 message encoding, enough for a stub resolver: one question, the answer section.
constexpr uint16_t kDnsTypeA = 1;
constexpr uint16_t kDnsTypeCname = 5;
constexpr uint16_t kDnsTypeAaaa = 28;
constexpr uint16_t kDnsClassIn = 1;

constexpr uint8_t kDnsRcodeNoError = 0;
constexpr uint8_t kDnsRcodeFormErr = 1;
constexpr uint8_t kDnsRcodeServFail = 2;
constexpr uint8_t kDnsRcodeNxDomain = 3;
constexpr uint8_t kDnsRcodeRefused = 5;

// A message is at most 512 bytes over UDP without EDNS0; over TCP it has a 16-bit length.
constexpr size_t kDnsUdpMaxMessage = 512;
constexpr size_t kDnsMaxName = 253;

struct DnsRecord {
    std::string name;
    uint16_t type = 0;
    uint16_t klass = 0;
    uint32_t ttl = 0;
    // Raw RDATA; for a CNAME, target holds the decoded name instead.
    std::string data;
    std::string target;
};

struct DnsMessage {
    uint16_t id = 0;
    bool response = false;
    bool truncated = false;
    uint8_t rcode = 0;
    // The single question.
    std::string qname;
    uint16_t qtype = 0;
    uint16_t qclass = 0;
    std::vector<DnsRecord> answers;
};

// Writes a recursive query for name, given without the trailing dot, to *out. Fails with
// ERROR_DNS_BAD_NAME for an empty label or a name or label over the RFC 1035 limits.
int EncodeDnsQuery(uint16_t id, const std::string &name, uint16_t qtype, std::string *out);

// Parses the header, one standard QUERY question and the answers; authority and additional
// sections are not read. Compressed names may only point backwards, so a pointer loop
// cannot hang the parser. Fails with ERROR_DNS_PROTOCOL on a malformed message.
int DecodeDnsMessage(const uint8_t *data, size_t size, DnsMessage *msg);

// Compares names ASCII case-insensitively, ignoring one trailing dot on either.
bool DnsNameEqual(const std::string &a, const std::string &b);

}  // namespace coco
