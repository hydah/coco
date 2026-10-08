#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "coco/net/dns/codec/config.hpp"
#include "coco/net/dns/codec/message.hpp"

namespace coco {

// Whether msg is a response to the question for name of type qtype.
bool DnsAnswersQuestion(const DnsMessage &msg, const std::string &name, uint16_t qtype);

// The A or AAAA addresses in msg for its question, following CNAMEs from the question
// name. Records owned by any other name are ignored, so a server cannot slip in unrelated
// addresses. *ttl is lowered to the smallest TTL on the way. Fails with ERROR_DNS_SERVER,
// and no addresses, when the CNAMEs loop.
int DnsAnswerAddresses(const DnsMessage &msg, uint16_t qtype, std::vector<IpAddress> *addrs,
                       uint32_t *ttl);

}  // namespace coco
