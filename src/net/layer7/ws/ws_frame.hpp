#pragma once

#include <stdint.h>
#include <stddef.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// websocket组合包最大不得超过4MB(防止内存爆炸)
#define MAX_WS_PACKET (4 * 1024 * 1024)

class WebSocketHeader {
 public:
    typedef enum {
        CONTINUATION = 0x0,
        TEXT = 0x1,
        BINARY = 0x2,
        RSV3 = 0x3,
        RSV4 = 0x4,
        RSV5 = 0x5,
        RSV6 = 0x6,
        RSV7 = 0x7,
        CLOSE = 0x8,
        PING = 0x9,
        PONG = 0xA,
        CONTROL_RSVB = 0xB,
        CONTROL_RSVC = 0xC,
        CONTROL_RSVD = 0xD,
        CONTROL_RSVE = 0xE,
        CONTROL_RSVF = 0xF
    } Type;

 public:
    // Starts with a fresh random masking key, as RFC 6455 requires for every client frame.
    WebSocketHeader();
    virtual ~WebSocketHeader() {}

 public:
    bool _fin = true;
    uint8_t _reserved = 0;
    Type _opcode = TEXT;
    bool _mask_flag = false;
    std::vector<uint8_t> _mask;
};

// A complete data message (fragments already joined) or a single control frame.
class WebSocektMessage {
 public:
    WebSocektMessage(){};
    virtual ~WebSocektMessage(){};

    // TEXT or BINARY for data messages, CLOSE / PING / PONG for control frames.
    WebSocketHeader::Type _opcode = WebSocketHeader::TEXT;
    // unmasked payload.
    std::string data_;
};

// Turns the bytes read from a connection into messages. Frames may be split across or
// packed into Decode() calls; fragmented data messages are joined, and control frames
// arriving between fragments are delivered on their own.
class WebSocketFrameDecoder {
 public:
    // A non-zero return stops decoding; Decode() returns it.
    typedef std::function<int(std::unique_ptr<WebSocektMessage> msg)> MessageHandler;

    explicit WebSocketFrameDecoder(MessageHandler on_message) : on_message_(on_message) {}

    // Returns ERROR_WS_PROTOCOL or ERROR_WS_MESSAGE_TOO_LARGE for a bad peer, or the
    // handler's error. After an error the decoder rejects all further input.
    int Decode(const uint8_t *data, size_t len);

 private:
    int OnFrame(bool fin, WebSocketHeader::Type opcode, const uint8_t *payload, size_t size,
                const uint8_t *mask);

    // received bytes that do not form a complete frame yet.
    std::string pending_;
    // the data message whose final fragment has not arrived.
    std::unique_ptr<WebSocektMessage> partial_;
    int err_ = 0;
    MessageHandler on_message_;
};

/**
 * 编码一个完整的数据包（头部 + 负载）
 * 带掩码时只对副本加掩码，调用方的 payload 保持不变
 */
std::string EncodeWebSocketFrame(const WebSocketHeader &header, const uint8_t *payload,
                                 size_t size);
