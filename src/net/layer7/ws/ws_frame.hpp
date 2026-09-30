#pragma once

#include <stdint.h>

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
    WebSocketHeader() : _mask(4) {
        //获取_mask内部buffer的内存地址，该内存是malloc开辟的，地址为随机
        uint64_t ptr = (uint64_t)(&_mask[0]);
        //根据内存地址设置掩码随机数
        _mask.assign((uint8_t *)(&ptr), (uint8_t *)(&ptr) + 4);
    }
    virtual ~WebSocketHeader() {}
    void Reset() {
        _fin = false;
        _reserved = 0;
        _opcode = CONTINUATION;
        _mask_flag = false;
        _payload_len = 0;
        _mask.clear();
        payload_offset_ = 0;
    }

 public:
    bool _fin;
    uint8_t _reserved;
    Type _opcode;
    bool _mask_flag;
    size_t _payload_len;
    std::vector<uint8_t> _mask;

    size_t payload_offset_ = 0;
};

class WebSocektMessage {
 public:
    WebSocektMessage(){};
    virtual ~WebSocektMessage(){};

    WebSocketHeader::Type _opcode;
    bool is_fragmented = false;
    std::string cache_;
    WebSocketHeader header_;
    bool got_header_ = false;

    std::string data_cache_;
    std::string data_;
};

// Turns a byte stream into WebSocket frames. Handles frames split across or packed into
// Decode() calls; each complete frame is handed to the callback.
class WebSocketFrameDecoder {
 public:
    typedef std::function<void(std::unique_ptr<WebSocektMessage> msg)> FrameHandler;

    explicit WebSocketFrameDecoder(FrameHandler on_frame) : on_frame_(on_frame) {}

    /**
     * 输入数据以便解包webSocket数据以及处理粘包问题
     * @param data 需要解包的数据，可能是不完整的包或多个包
     * @param len 数据长度
     */
    void Decode(uint8_t *data, size_t len);

    // Called from the frame handler with a non-final data frame: the following frames
    // are decoded into it until the message is complete.
    void Continue(std::unique_ptr<WebSocektMessage> msg, bool mask_flag);

 private:
    std::unique_ptr<WebSocektMessage> cur_msg_;
    int _mask_offset = 0;
    FrameHandler on_frame_;
};

/**
 * 编码一个数据包的头部
 * @param header 数据头
 * @param buffer 负载数据，带掩码时原地加掩码
 */
std::string EncodeWebSocketFrameHeader(WebSocketHeader &header, uint8_t *buffer, uint32_t size);
