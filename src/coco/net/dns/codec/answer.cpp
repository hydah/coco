#include "coco/net/dns/codec/answer.hpp"

#include <string.h>

#include <algorithm>
#include <map>
#include <set>

#include "coco/common/error.hpp"

namespace coco {

bool DnsAnswersQuestion(const DnsMessage &msg, const std::string &name, uint16_t qtype) {
    return msg.response && msg.qtype == qtype && msg.qclass == kDnsClassIn &&
           DnsNameEqual(msg.qname, name);
}

int DnsAnswerAddresses(const DnsMessage &msg, uint16_t qtype, std::vector<IpAddress> *addrs,
                       uint32_t *ttl) {
    addrs->clear();
    std::map<std::string, const DnsRecord *> aliases;
    for (const DnsRecord &rr : msg.answers) {
        if (rr.type == kDnsTypeCname && rr.klass == kDnsClassIn) {
            aliases.emplace(DnsLower(rr.name), &rr);
        }
    }
    std::string current = DnsLower(msg.qname);
    std::set<std::string> names{current};
    for (;;) {
        auto alias = aliases.find(current);
        if (alias == aliases.end()) {
            break;
        }
        current = DnsLower(alias->second->target);
        if (!names.insert(current).second) {
            return ERROR_DNS_SERVER;
        }
        *ttl = std::min(*ttl, alias->second->ttl);
    }

    size_t size = qtype == kDnsTypeA ? 4 : 16;
    for (const DnsRecord &rr : msg.answers) {
        if (rr.type != qtype || rr.klass != kDnsClassIn || rr.data.size() != size) {
            continue;
        }
        if (names.count(DnsLower(rr.name)) == 0) {
            continue;
        }
        IpAddress ip;
        ip.family = qtype == kDnsTypeA ? AF_INET : AF_INET6;
        memcpy(ip.bytes, rr.data.data(), size);
        addrs->push_back(ip);
        *ttl = std::min(*ttl, rr.ttl);
    }
    return COCO_SUCCESS;
}

}  // namespace coco
