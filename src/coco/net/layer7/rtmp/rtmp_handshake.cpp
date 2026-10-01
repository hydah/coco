#include "coco/net/layer7/rtmp/rtmp_handshake.hpp"

#include <string.h>
#include <time.h>

#include <random>
#include <string>

#include "coco/common/error.hpp"
#include "coco/log/log.hpp"

namespace coco {

namespace {

constexpr int kSigSize = 1536;
constexpr int kDigestSize = 32;

// The well-known digest keys of the RTMP complex handshake. Each is the ASCII label
// followed by the same 32-byte suffix. C1/S1 are signed with the label alone; C2/S2
// with the label plus the suffix.
const char kFpLabel[] = "Genuine Adobe Flash Player 001";         // 30
const char kFmsLabel[] = "Genuine Adobe Flash Media Server 001";  // 36
const uint8_t kKeySuffix[32] = {0xF0, 0xEE, 0xC2, 0x4A, 0x80, 0x68, 0xBE, 0xE8, 0x2E, 0x00, 0xD0,
                                0xD1, 0x02, 0x9E, 0x7E, 0x57, 0x6E, 0xEC, 0x5D, 0x2D, 0x29, 0x80,
                                0x6F, 0xAB, 0x93, 0xB8, 0xE6, 0x36, 0xCF, 0xEB, 0x31, 0xAE};

const uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void Sha256Block(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + kSha256K[i] + w[i];
        uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void Sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block[64];
    size_t off = 0;
    while (off + 64 <= len) {
        Sha256Block(h, data + off);
        off += 64;
    }
    size_t rem = len - off;
    memset(block, 0, 64);
    if (rem) {
        memcpy(block, data + off, rem);
    }
    block[rem] = 0x80;
    if (rem >= 56) {
        Sha256Block(h, block);
        memset(block, 0, 64);
    }
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        block[63 - i] = (uint8_t)(bits >> (i * 8));
    }
    Sha256Block(h, block);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

void HmacSha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len,
                uint8_t out[32]) {
    uint8_t k[64];
    memset(k, 0, 64);
    if (key_len > 64) {
        Sha256(key, key_len, k);
    } else if (key_len) {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    std::string inner(reinterpret_cast<char*>(ipad), 64);
    inner.append(reinterpret_cast<const char*>(data), data_len);
    uint8_t mid[32];
    Sha256(reinterpret_cast<const uint8_t*>(inner.data()), inner.size(), mid);
    std::string outer(reinterpret_cast<char*>(opad), 64);
    outer.append(reinterpret_cast<char*>(mid), 32);
    Sha256(reinterpret_cast<const uint8_t*>(outer.data()), outer.size(), out);
}

int ReadFull(IoReader* in, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t k = 0;
        int ret = in->Read(p + got, n - got, &k);
        if (ret != COCO_SUCCESS) {
            return ret;
        }
        if (k <= 0) {
            return ERROR_SOCKET_READ;
        }
        got += (size_t)k;
    }
    return COCO_SUCCESS;
}

int WriteFull(IoWriter* out, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t k = 0;
        int ret = out->Write((void*)(p + got), n - got, &k);
        if (ret != COCO_SUCCESS) {
            return ret;
        }
        if (k <= 0) {
            return ERROR_SOCKET_WRITE;
        }
        got += (size_t)k;
    }
    return COCO_SUCCESS;
}

void FillRandom(uint8_t* p, size_t n) {
    std::random_device rd;
    for (size_t i = 0; i < n;) {
        uint32_t v = rd();
        size_t c = n - i < 4 ? n - i : 4;
        memcpy(p + i, &v, c);
        i += c;
    }
}

int DigestOffset(const uint8_t* buf, int base) {
    int sum = buf[base] + buf[base + 1] + buf[base + 2] + buf[base + 3];
    // scheme 0: bytes 8..11, digest in the first half. scheme 1: bytes 772..775.
    if (base == 8) {
        return (sum % 728) + 12;
    }
    return (sum % 728) + 776;
}

// HMAC of the 1536-byte packet with the 32-byte digest slot removed.
void PacketDigest(const uint8_t* pkt, int off, const uint8_t* key, size_t key_len,
                  uint8_t out[32]) {
    std::string msg;
    msg.append(reinterpret_cast<const char*>(pkt), off);
    msg.append(reinterpret_cast<const char*>(pkt + off + kDigestSize),
               kSigSize - off - kDigestSize);
    HmacSha256(key, key_len, reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), out);
}

bool DigestOk(const uint8_t* pkt, int off, const uint8_t* key, size_t key_len) {
    if (off < 0 || off + kDigestSize > kSigSize) {
        return false;
    }
    uint8_t dig[32];
    PacketDigest(pkt, off, key, key_len, dig);
    return memcmp(dig, pkt + off, kDigestSize) == 0;
}

// Finds which scheme signed pkt. digest_out receives the 32-byte digest.
bool FindDigest(const uint8_t* pkt, const uint8_t* key, size_t key_len, int* scheme,
                uint8_t digest_out[32]) {
    const int bases[2] = {8, 772};
    for (int s = 0; s < 2; s++) {
        int off = DigestOffset(pkt, bases[s]);
        if (DigestOk(pkt, off, key, key_len)) {
            *scheme = s;
            memcpy(digest_out, pkt + off, kDigestSize);
            return true;
        }
    }
    return false;
}

void SignPacket(uint8_t pkt[kSigSize], int scheme, const uint8_t* key, size_t key_len) {
    int off = DigestOffset(pkt, scheme == 0 ? 8 : 772);
    uint8_t dig[32];
    PacketDigest(pkt, off, key, key_len, dig);
    memcpy(pkt + off, dig, kDigestSize);
}

void FillSigned(uint8_t pkt[kSigSize], int scheme, const uint8_t* key, size_t key_len,
                uint32_t version) {
    FillRandom(pkt, kSigSize);
    uint32_t now = (uint32_t)time(nullptr);
    pkt[0] = (uint8_t)(now >> 24);
    pkt[1] = (uint8_t)(now >> 16);
    pkt[2] = (uint8_t)(now >> 8);
    pkt[3] = (uint8_t)now;
    pkt[4] = (uint8_t)(version >> 24);
    pkt[5] = (uint8_t)(version >> 16);
    pkt[6] = (uint8_t)(version >> 8);
    pkt[7] = (uint8_t)version;
    SignPacket(pkt, scheme, key, key_len);
}

// C2 or S2: HMAC(key, peer_digest) is the key for HMAC over 1504 random bytes.
void MakeResponse(const uint8_t* key, size_t key_len, const uint8_t peer_digest[32],
                  uint8_t out[kSigSize]) {
    uint8_t temp[32];
    HmacSha256(key, key_len, peer_digest, kDigestSize, temp);
    FillRandom(out, kSigSize - kDigestSize);
    HmacSha256(temp, 32, out, kSigSize - kDigestSize, out + kSigSize - kDigestSize);
}

void FullKey(const char* label, size_t label_len, uint8_t* out) {
    memcpy(out, label, label_len);
    memcpy(out + label_len, kKeySuffix, 32);
}

bool VersionZero(const uint8_t* pkt) {
    return pkt[4] == 0 && pkt[5] == 0 && pkt[6] == 0 && pkt[7] == 0;
}

}  // namespace

bool RtmpDigestSelfCheck() {
    uint8_t out[32];
    Sha256(reinterpret_cast<const uint8_t*>(""), 0, out);
    const uint8_t empty[32] = {0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
                               0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
                               0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
    if (memcmp(out, empty, 32) != 0) {
        return false;
    }
    Sha256(reinterpret_cast<const uint8_t*>("abc"), 3, out);
    const uint8_t abc[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                             0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                             0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    if (memcmp(out, abc, 32) != 0) {
        return false;
    }
    // RFC 4231 test case 1.
    uint8_t key[20];
    memset(key, 0x0b, 20);
    const uint8_t data[] = {'H', 'i', ' ', 'T', 'h', 'e', 'r', 'e'};
    HmacSha256(key, 20, data, sizeof(data), out);
    const uint8_t hmac[32] = {0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf,
                              0xce, 0xaf, 0x0b, 0xf1, 0x2b, 0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83,
                              0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7};
    return memcmp(out, hmac, 32) == 0;
}

int RtmpHandshake(IoReader* in, IoWriter* out, bool client) {
    const size_t fp_len = sizeof(kFpLabel) - 1;
    const size_t fms_len = sizeof(kFmsLabel) - 1;
    uint8_t fp_key[62], fms_key[68];
    FullKey(kFpLabel, fp_len, fp_key);
    FullKey(kFmsLabel, fms_len, fms_key);

    if (client) {
        uint8_t c0c1[1 + kSigSize];
        c0c1[0] = 3;
        FillSigned(c0c1 + 1, 0, reinterpret_cast<const uint8_t*>(kFpLabel), fp_len, 0x80000702);
        int ret = WriteFull(out, c0c1, sizeof(c0c1));
        if (ret != COCO_SUCCESS) {
            return ret;
        }
        uint8_t s0s1s2[1 + kSigSize * 2];
        if ((ret = ReadFull(in, s0s1s2, sizeof(s0s1s2))) != COCO_SUCCESS) {
            return ret;
        }
        if (s0s1s2[0] != 3) {
            coco_error("rtmp: server handshake version %u", s0s1s2[0]);
            return ERROR_RTMP_HANDSHAKE;
        }
        uint8_t c2[kSigSize];
        int scheme = 0;
        uint8_t digest[32];
        if (!VersionZero(s0s1s2 + 1) &&
            FindDigest(s0s1s2 + 1, reinterpret_cast<const uint8_t*>(kFmsLabel), fms_len, &scheme,
                       digest)) {
            MakeResponse(fp_key, sizeof(fp_key), digest, c2);
        } else {
            memcpy(c2, s0s1s2 + 1, kSigSize);
        }
        return WriteFull(out, c2, sizeof(c2));
    }

    uint8_t c0c1[1 + kSigSize];
    int ret = ReadFull(in, c0c1, sizeof(c0c1));
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    if (c0c1[0] != 3) {
        coco_error("rtmp: client handshake version %u", c0c1[0]);
        return ERROR_RTMP_HANDSHAKE;
    }
    int scheme = 0;
    uint8_t digest[32];
    bool complex =
        !VersionZero(c0c1 + 1) &&
        FindDigest(c0c1 + 1, reinterpret_cast<const uint8_t*>(kFpLabel), fp_len, &scheme, digest);
    uint8_t s0s1s2[1 + kSigSize * 2];
    s0s1s2[0] = 3;
    if (complex) {
        FillSigned(s0s1s2 + 1, scheme, reinterpret_cast<const uint8_t*>(kFmsLabel), fms_len,
                   0x0D0E0A0D);
        MakeResponse(fms_key, sizeof(fms_key), digest, s0s1s2 + 1 + kSigSize);
    } else {
        coco_trace("rtmp: simple handshake");
        FillRandom(s0s1s2 + 1, kSigSize);
        uint32_t now = (uint32_t)time(nullptr);
        s0s1s2[1] = (uint8_t)(now >> 24);
        s0s1s2[2] = (uint8_t)(now >> 16);
        s0s1s2[3] = (uint8_t)(now >> 8);
        s0s1s2[4] = (uint8_t)now;
        memset(s0s1s2 + 5, 0, 4);  // zero version: the peer treats S2 as an echo
        memcpy(s0s1s2 + 1 + kSigSize, c0c1 + 1, kSigSize);
    }
    if ((ret = WriteFull(out, s0s1s2, sizeof(s0s1s2))) != COCO_SUCCESS) {
        return ret;
    }
    uint8_t c2[kSigSize];
    return ReadFull(in, c2, sizeof(c2));
}

}  // namespace coco
