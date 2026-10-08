#include "coco/net/dns/codec/message.hpp"

#include "coco/common/error.hpp"

namespace coco {

namespace {

const size_t kHeaderSize = 12;
const size_t kMaxLabel = 63;
// A name on the wire, length bytes and the root label included, is at most 255 bytes.
const size_t kMaxWireName = 255;

void PutU16(std::string *out, uint16_t v) {
    out->push_back((char)(v >> 8));
    out->push_back((char)(v & 0xff));
}

class Reader {
 public:
    Reader(const uint8_t *data, size_t size) : data_(data), size_(size) {}

    size_t pos() const { return pos_; }

    bool U16(uint16_t *v) {
        if (size_ - pos_ < 2) {
            return false;
        }
        *v = (uint16_t)((data_[pos_] << 8) | data_[pos_ + 1]);
        pos_ += 2;
        return true;
    }

    bool U32(uint32_t *v) {
        uint16_t hi = 0, lo = 0;
        if (!U16(&hi) || !U16(&lo)) {
            return false;
        }
        *v = ((uint32_t)hi << 16) | lo;
        return true;
    }

    bool Bytes(size_t n, std::string *out) {
        if (size_ - pos_ < n) {
            return false;
        }
        out->assign((const char *)data_ + pos_, n);
        pos_ += n;
        return true;
    }

    // Reads a possibly compressed name, as dotted text without the trailing dot ("" for
    // the root). Every pointer must go strictly backwards from where it was found.
    bool Name(std::string *out) {
        out->clear();
        size_t p = pos_;
        size_t wire = 0;
        size_t steps = 0;
        bool jumped = false;
        for (;;) {
            if (p >= size_ || ++steps > kMaxWireName) {
                return false;
            }
            uint8_t len = data_[p];
            if ((len & 0xc0) == 0xc0) {
                if (p + 1 >= size_) {
                    return false;
                }
                size_t target = ((size_t)(len & 0x3f) << 8) | data_[p + 1];
                if (target >= p) {
                    return false;
                }
                if (!jumped) {
                    pos_ = p + 2;
                    jumped = true;
                }
                p = target;
                continue;
            }
            if ((len & 0xc0) != 0) {
                return false;
            }
            wire += 1 + len;
            if (wire > kMaxWireName) {
                return false;
            }
            if (len == 0) {
                if (!jumped) {
                    pos_ = p + 1;
                }
                return true;
            }
            if (p + 1 + len > size_) {
                return false;
            }
            for (size_t label_pos = p + 1; label_pos < p + 1 + len; ++label_pos) {
                if (data_[label_pos] == '.' || data_[label_pos] == '\0') {
                    return false;
                }
            }
            if (!out->empty()) {
                out->push_back('.');
            }
            out->append((const char *)data_ + p + 1, len);
            p += 1 + len;
        }
    }

 private:
    const uint8_t *data_;
    size_t size_;
    size_t pos_ = 0;
};

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

size_t TrimmedSize(const std::string &s) {
    return (!s.empty() && s.back() == '.') ? s.size() - 1 : s.size();
}

}  // namespace

int EncodeDnsQuery(uint16_t id, const std::string &name, uint16_t qtype, std::string *out) {
    if (name.empty() || name.size() > kDnsMaxName) {
        return ERROR_DNS_BAD_NAME;
    }
    out->clear();
    PutU16(out, id);
    PutU16(out, 0x0100);  // RD: ask the server to recurse.
    PutU16(out, 1);       // QDCOUNT
    PutU16(out, 0);
    PutU16(out, 0);
    PutU16(out, 0);

    size_t start = 0;
    while (start <= name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) {
            dot = name.size();
        }
        size_t len = dot - start;
        if (len == 0 || len > kMaxLabel) {
            return ERROR_DNS_BAD_NAME;
        }
        out->push_back((char)len);
        out->append(name, start, len);
        start = dot + 1;
    }
    out->push_back('\0');
    PutU16(out, qtype);
    PutU16(out, kDnsClassIn);
    return COCO_SUCCESS;
}

int DecodeDnsMessage(const uint8_t *data, size_t size, DnsMessage *msg) {
    if (size < kHeaderSize) {
        return ERROR_DNS_PROTOCOL;
    }
    Reader r(data, size);
    uint16_t flags = 0, qdcount = 0, ancount = 0, nscount = 0, arcount = 0;
    r.U16(&msg->id);
    r.U16(&flags);
    r.U16(&qdcount);
    r.U16(&ancount);
    r.U16(&nscount);
    r.U16(&arcount);
    if ((flags & 0x7800) != 0 || qdcount != 1) {
        return ERROR_DNS_PROTOCOL;
    }
    msg->response = (flags & 0x8000) != 0;
    msg->truncated = (flags & 0x0200) != 0;
    msg->rcode = (uint8_t)(flags & 0x000f);
    msg->qname.clear();
    msg->qtype = msg->qclass = 0;
    msg->answers.clear();

    for (uint16_t i = 0; i < qdcount; ++i) {
        std::string name;
        uint16_t qtype = 0, qclass = 0;
        if (!r.Name(&name) || !r.U16(&qtype) || !r.U16(&qclass)) {
            return ERROR_DNS_PROTOCOL;
        }
        if (i == 0) {
            msg->qname = name;
            msg->qtype = qtype;
            msg->qclass = qclass;
        }
    }

    for (uint16_t i = 0; i < ancount; ++i) {
        DnsRecord rr;
        uint16_t rdlength = 0;
        if (!r.Name(&rr.name) || !r.U16(&rr.type) || !r.U16(&rr.klass) || !r.U32(&rr.ttl) ||
            !r.U16(&rdlength)) {
            return ERROR_DNS_PROTOCOL;
        }
        size_t rdata_start = r.pos();
        if (rr.type == kDnsTypeCname) {
            if (!r.Name(&rr.target) || r.pos() != rdata_start + rdlength) {
                return ERROR_DNS_PROTOCOL;
            }
        } else if (!r.Bytes(rdlength, &rr.data)) {
            return ERROR_DNS_PROTOCOL;
        }
        // The high bit of a TTL is reserved; RFC 2181 says to treat such a TTL as zero.
        if (rr.ttl & 0x80000000u) {
            rr.ttl = 0;
        }
        msg->answers.push_back(rr);
    }
    return COCO_SUCCESS;
}

bool DnsNameEqual(const std::string &a, const std::string &b) {
    size_t n = TrimmedSize(a);
    if (n != TrimmedSize(b)) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (Lower(a[i]) != Lower(b[i])) {
            return false;
        }
    }
    return true;
}

std::string DnsLower(const std::string &name) {
    std::string s = name;
    for (char &c : s) {
        c = Lower(c);
    }
    return s;
}

}  // namespace coco
