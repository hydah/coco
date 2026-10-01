#include "coco/net/layer7/rtmp/rtmp_amf0.hpp"

#include "coco/common/error.hpp"
#include "coco/net/layer7/rtmp/rtmp_bytes.hpp"

namespace coco {

namespace {

constexpr int kMaxDepth = 32;
constexpr size_t kMaxElements = 100000;

int DecodeOne(const uint8_t* p, size_t n, Amf0Value* out, size_t* used, int depth);

int DecodeProps(const uint8_t* p, size_t n, size_t off, Amf0Value* out, int depth, size_t* end) {
    while (true) {
        if (off + 2 > n) {
            return ERROR_RTMP_AMF;
        }
        uint16_t nlen = RtmpBe16(p + off);
        off += 2;
        if (nlen == 0) {
            if (off >= n || p[off] != 0x09) {
                return ERROR_RTMP_AMF;
            }
            *end = off + 1;
            return COCO_SUCCESS;
        }
        if (off + nlen > n) {
            return ERROR_RTMP_AMF;
        }
        std::string key((const char*)p + off, nlen);
        off += nlen;
        Amf0Value child;
        size_t child_used = 0;
        int ret = DecodeOne(p + off, n - off, &child, &child_used, depth + 1);
        if (ret != COCO_SUCCESS) {
            return ret;
        }
        off += child_used;
        out->props.push_back(std::make_pair(key, child));
        if (out->props.size() > kMaxElements) {
            return ERROR_RTMP_AMF;
        }
    }
}

int DecodeOne(const uint8_t* p, size_t n, Amf0Value* out, size_t* used, int depth) {
    if (depth > kMaxDepth || n < 1) {
        return ERROR_RTMP_AMF;
    }
    uint8_t marker = p[0];
    const uint8_t* b = p + 1;
    size_t left = n - 1;
    switch (marker) {
        case Amf0Value::kNumber:
            if (left < 8) {
                return ERROR_RTMP_AMF;
            }
            out->type = Amf0Value::kNumber;
            out->number = RtmpBeDouble(b);
            *used = 9;
            return COCO_SUCCESS;
        case Amf0Value::kBoolean:
            if (left < 1) {
                return ERROR_RTMP_AMF;
            }
            out->type = Amf0Value::kBoolean;
            out->boolean = b[0] != 0;
            *used = 2;
            return COCO_SUCCESS;
        case Amf0Value::kString: {
            if (left < 2) {
                return ERROR_RTMP_AMF;
            }
            uint16_t len = RtmpBe16(b);
            if (left < (size_t)2 + len) {
                return ERROR_RTMP_AMF;
            }
            out->type = Amf0Value::kString;
            out->str.assign((const char*)b + 2, len);
            *used = 1 + 2 + len;
            return COCO_SUCCESS;
        }
        case Amf0Value::kObject:
        case Amf0Value::kEcmaArray: {
            size_t off = 1;
            if (marker == Amf0Value::kEcmaArray) {
                if (left < 4) {
                    return ERROR_RTMP_AMF;
                }
                off += 4;  // the count is advisory; the end marker terminates the array
            }
            out->type =
                marker == Amf0Value::kEcmaArray ? Amf0Value::kEcmaArray : Amf0Value::kObject;
            out->props.clear();
            size_t end = 0;
            int ret = DecodeProps(p, n, off, out, depth, &end);
            if (ret != COCO_SUCCESS) {
                return ret;
            }
            *used = end;
            return COCO_SUCCESS;
        }
        case Amf0Value::kNull:
            out->type = Amf0Value::kNull;
            *used = 1;
            return COCO_SUCCESS;
        case Amf0Value::kUndefined:
        case 0x0D:  // unsupported, no payload
            out->type = Amf0Value::kUndefined;
            *used = 1;
            return COCO_SUCCESS;
        case Amf0Value::kStrictArray: {
            if (left < 4) {
                return ERROR_RTMP_AMF;
            }
            uint32_t count = RtmpBe32(b);
            if (count > kMaxElements) {
                return ERROR_RTMP_AMF;
            }
            out->type = Amf0Value::kStrictArray;
            out->items.clear();
            size_t off = 5;
            for (uint32_t i = 0; i < count; i++) {
                if (off >= n) {
                    return ERROR_RTMP_AMF;
                }
                Amf0Value child;
                size_t child_used = 0;
                int ret = DecodeOne(p + off, n - off, &child, &child_used, depth + 1);
                if (ret != COCO_SUCCESS) {
                    return ret;
                }
                off += child_used;
                out->items.push_back(child);
            }
            *used = off;
            return COCO_SUCCESS;
        }
        case 0x0B:  // date: number plus a timezone the command path does not use
            if (left < 10) {
                return ERROR_RTMP_AMF;
            }
            out->type = Amf0Value::kNumber;
            out->number = RtmpBeDouble(b);
            *used = 11;
            return COCO_SUCCESS;
        case 0x0C: {
            if (left < 4) {
                return ERROR_RTMP_AMF;
            }
            uint32_t len = RtmpBe32(b);
            if (left - 4 < len) {
                return ERROR_RTMP_AMF;
            }
            out->type = Amf0Value::kString;
            out->str.assign((const char*)b + 4, len);
            *used = 1 + 4 + len;
            return COCO_SUCCESS;
        }
        default:
            return ERROR_RTMP_AMF;
    }
}

void EncodeOne(const Amf0Value& v, std::string* o);

void EncodeProps(const Amf0Value& v, std::string* o) {
    for (size_t i = 0; i < v.props.size(); i++) {
        const std::string& key = v.props[i].first;
        if (key.empty() || key.size() > 0xffff) {
            continue;
        }
        RtmpPutBe16(o, (uint16_t)key.size());
        o->append(key);
        EncodeOne(v.props[i].second, o);
    }
    o->push_back(0);
    o->push_back(0);
    o->push_back(0x09);
}

void EncodeOne(const Amf0Value& v, std::string* o) {
    switch (v.type) {
        case Amf0Value::kNumber:
            o->push_back(Amf0Value::kNumber);
            RtmpPutBeDouble(o, v.number);
            break;
        case Amf0Value::kBoolean:
            o->push_back(Amf0Value::kBoolean);
            o->push_back(v.boolean ? 1 : 0);
            break;
        case Amf0Value::kString:
            if (v.str.size() > 0xffff) {
                o->push_back(0x0C);
                RtmpPutBe32(o, (uint32_t)v.str.size());
            } else {
                o->push_back(Amf0Value::kString);
                RtmpPutBe16(o, (uint16_t)v.str.size());
            }
            o->append(v.str);
            break;
        case Amf0Value::kObject:
            o->push_back(Amf0Value::kObject);
            EncodeProps(v, o);
            break;
        case Amf0Value::kEcmaArray:
            o->push_back(Amf0Value::kEcmaArray);
            RtmpPutBe32(o, (uint32_t)v.props.size());
            EncodeProps(v, o);
            break;
        case Amf0Value::kStrictArray:
            o->push_back(Amf0Value::kStrictArray);
            RtmpPutBe32(o, (uint32_t)v.items.size());
            for (size_t i = 0; i < v.items.size(); i++) {
                EncodeOne(v.items[i], o);
            }
            break;
        case Amf0Value::kUndefined:
            o->push_back(Amf0Value::kUndefined);
            break;
        case Amf0Value::kNull:
        default:
            o->push_back(Amf0Value::kNull);
            break;
    }
}

}  // namespace

Amf0Value Amf0Value::Num(double v) {
    Amf0Value a;
    a.type = kNumber;
    a.number = v;
    return a;
}

Amf0Value Amf0Value::Bool(bool v) {
    Amf0Value a;
    a.type = kBoolean;
    a.boolean = v;
    return a;
}

Amf0Value Amf0Value::Str(const std::string& v) {
    Amf0Value a;
    a.type = kString;
    a.str = v;
    return a;
}

Amf0Value Amf0Value::Null() {
    Amf0Value a;
    a.type = kNull;
    return a;
}

Amf0Value Amf0Value::Undef() {
    Amf0Value a;
    a.type = kUndefined;
    return a;
}

Amf0Value Amf0Value::Object() {
    Amf0Value a;
    a.type = kObject;
    return a;
}

Amf0Value Amf0Value::Ecma() {
    Amf0Value a;
    a.type = kEcmaArray;
    return a;
}

Amf0Value Amf0Value::Array() {
    Amf0Value a;
    a.type = kStrictArray;
    return a;
}

Amf0Value& Amf0Value::Put(const std::string& key, const Amf0Value& v) {
    props.push_back(std::make_pair(key, v));
    return *this;
}

Amf0Value& Amf0Value::Push(const Amf0Value& v) {
    items.push_back(v);
    return *this;
}

const Amf0Value* Amf0Value::Find(const std::string& key) const {
    for (size_t i = 0; i < props.size(); i++) {
        if (props[i].first == key) {
            return &props[i].second;
        }
    }
    return nullptr;
}

std::string Amf0Value::GetString(const std::string& key, const std::string& def) const {
    const Amf0Value* v = Find(key);
    if (v == nullptr || v->type != kString) {
        return def;
    }
    return v->str;
}

double Amf0Value::GetNumber(const std::string& key, double def) const {
    const Amf0Value* v = Find(key);
    if (v == nullptr || v->type != kNumber) {
        return def;
    }
    return v->number;
}

std::string Amf0Encode(const Amf0Value& v) {
    std::string o;
    EncodeOne(v, &o);
    return o;
}

std::string Amf0Encode(const std::vector<Amf0Value>& values) {
    std::string o;
    for (size_t i = 0; i < values.size(); i++) {
        EncodeOne(values[i], &o);
    }
    return o;
}

int Amf0Decode(const uint8_t* p, size_t n, Amf0Value* out, size_t* used) {
    return DecodeOne(p, n, out, used, 0);
}

int Amf0DecodeAll(const uint8_t* p, size_t n, std::vector<Amf0Value>* out) {
    size_t off = 0;
    out->clear();
    while (off < n) {
        Amf0Value v;
        size_t used = 0;
        int ret = DecodeOne(p + off, n - off, &v, &used, 0);
        if (ret != COCO_SUCCESS) {
            return ret;
        }
        if (used == 0) {
            return ERROR_RTMP_AMF;
        }
        off += used;
        out->push_back(v);
        if (out->size() > kMaxElements) {
            return ERROR_RTMP_AMF;
        }
    }
    return COCO_SUCCESS;
}

}  // namespace coco
