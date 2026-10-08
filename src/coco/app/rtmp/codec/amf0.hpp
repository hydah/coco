#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

namespace coco {

// One AMF0 value. Objects and ECMA arrays keep named properties in order; strict
// arrays use items. A long string is stored as kString and written back as a long
// string when it does not fit in 16 bits.
class Amf0Value {
 public:
    enum Type {
        kNumber = 0x00,
        kBoolean = 0x01,
        kString = 0x02,
        kObject = 0x03,
        kNull = 0x05,
        kUndefined = 0x06,
        kEcmaArray = 0x08,
        kStrictArray = 0x0A,
    };

    Type type = kNull;
    double number = 0;
    bool boolean = false;
    std::string str;
    std::vector<std::pair<std::string, Amf0Value>> props;
    std::vector<Amf0Value> items;

    static Amf0Value Num(double v);
    static Amf0Value Bool(bool v);
    static Amf0Value Str(const std::string& v);
    static Amf0Value Null();
    static Amf0Value Undef();
    static Amf0Value Object();
    static Amf0Value Ecma();
    static Amf0Value Array();

    Amf0Value& Put(const std::string& key, const Amf0Value& v);
    Amf0Value& Push(const Amf0Value& v);
    const Amf0Value* Find(const std::string& key) const;
    std::string GetString(const std::string& key, const std::string& def = "") const;
    double GetNumber(const std::string& key, double def = 0) const;
};

// Encodes one value, or a sequence (an RTMP command is a sequence).
std::string Amf0Encode(const Amf0Value& v);
std::string Amf0Encode(const std::vector<Amf0Value>& values);

// Decodes one value at p. *used is how many bytes it consumed.
int Amf0Decode(const uint8_t* p, size_t n, Amf0Value* out, size_t* used);
// Decodes values until the buffer is empty. Trailing junk is ERROR_RTMP_AMF.
int Amf0DecodeAll(const uint8_t* p, size_t n, std::vector<Amf0Value>* out);

}  // namespace coco
