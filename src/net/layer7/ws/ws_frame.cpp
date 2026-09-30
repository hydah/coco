#include "net/layer7/ws/ws_frame.hpp"

#include <random>

#include "common/error.hpp"

/*
  0             1                 2               3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-------+-+-------------+-------------------------------+
 |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
 |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
 |N|V|V|V|       |S|             |   (if payload len==126/127)   |
 | |1|2|3|       |K|             |                               |
 +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
 |     Extended payload length continued, if payload len == 127  |
 + - - - - - - - - - - - - - - - +-------------------------------+
 |                               |Masking-key, if MASK set to 1  |
 +-------------------------------+-------------------------------+
 | Masking-key (continued)       |          Payload Data         |
 +-------------------------------- - - - - - - - - - - - - - - - +
 :                     Payload Data                              :
 + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
 |                     Payload Data                              |
 +---------------------------------------------------------------+
 */

// RFC 6455 5.5: control frames carry at most 125 bytes and are never fragmented.
static const size_t kMaxControlPayload = 125;

static bool IsControl(int opcode) { return (opcode & 0x08) != 0; }

WebSocketHeader::WebSocketHeader() : _mask(4) {
    // The key must be unpredictable to intermediaries, so it comes from the OS entropy.
    static std::random_device rd;
    uint32_t key = rd();
    for (int i = 0; i < 4; ++i) {
        _mask[i] = (uint8_t)(key >> (8 * i));
    }
}

int WebSocketFrameDecoder::Decode(const uint8_t *data, size_t len) {
    if (err_ != 0) {
        return err_;
    }
    pending_.append((const char *)data, len);

    size_t pos = 0;
    while (true) {
        const uint8_t *p = (const uint8_t *)pending_.data() + pos;
        size_t avail = pending_.size() - pos;

        if (avail < 2) {
            break;
        }
        bool fin = (p[0] & 0x80) != 0;
        uint8_t reserved = (p[0] >> 4) & 0x07;
        int opcode = p[0] & 0x0F;
        bool masked = (p[1] & 0x80) != 0;
        uint64_t size = p[1] & 0x7F;
        size_t header_len = 2;

        if (size == 126) {
            if (avail < 4) {
                break;
            }
            size = ((uint64_t)p[2] << 8) | p[3];
            header_len = 4;
        } else if (size == 127) {
            if (avail < 10) {
                break;
            }
            size = 0;
            for (int i = 0; i < 8; ++i) {
                size = (size << 8) | p[2 + i];
            }
            header_len = 10;
        }
        if (masked) {
            header_len += 4;
        }

        // Reject before buffering the payload, so a bogus length cannot grow pending_.
        // No extension is negotiated, so the RSV bits must be zero.
        bool known = opcode <= WebSocketHeader::BINARY || (opcode >= WebSocketHeader::CLOSE &&
                                                           opcode <= WebSocketHeader::PONG);
        if (reserved != 0 || !known) {
            err_ = ERROR_WS_PROTOCOL;
            return err_;
        }
        if (IsControl(opcode) && (!fin || size > kMaxControlPayload)) {
            err_ = ERROR_WS_PROTOCOL;
            return err_;
        }
        if (size > MAX_WS_PACKET) {
            err_ = ERROR_WS_MESSAGE_TOO_LARGE;
            return err_;
        }

        if (avail < header_len + size) {
            break;
        }
        const uint8_t *mask = masked ? p + header_len - 4 : nullptr;
        int ret = OnFrame(fin, (WebSocketHeader::Type)opcode, p + header_len, (size_t)size, mask);
        if (ret != 0) {
            err_ = ret;
            return err_;
        }
        pos += header_len + size;
    }

    pending_.erase(0, pos);
    return 0;
}

int WebSocketFrameDecoder::OnFrame(bool fin, WebSocketHeader::Type opcode,
                                   const uint8_t *payload, size_t size, const uint8_t *mask) {
    std::unique_ptr<WebSocektMessage> control;
    WebSocektMessage *msg = nullptr;

    if (IsControl(opcode)) {
        control.reset(new WebSocektMessage());
        control->_opcode = opcode;
        msg = control.get();
    } else if (opcode == WebSocketHeader::CONTINUATION) {
        if (!partial_) {
            return ERROR_WS_PROTOCOL;
        }
        if (partial_->data_.size() + size > MAX_WS_PACKET) {
            return ERROR_WS_MESSAGE_TOO_LARGE;
        }
        msg = partial_.get();
    } else {
        // A new data message must not start before the previous one is finished.
        if (partial_) {
            return ERROR_WS_PROTOCOL;
        }
        partial_.reset(new WebSocektMessage());
        partial_->_opcode = opcode;
        msg = partial_.get();
    }

    size_t offset = msg->data_.size();
    msg->data_.append((const char *)payload, size);
    if (mask) {
        for (size_t i = 0; i < size; ++i) {
            msg->data_[offset + i] ^= mask[i % 4];
        }
    }

    if (control) {
        return on_message_(std::move(control));
    }
    if (fin) {
        return on_message_(std::move(partial_));
    }
    return 0;
}

std::string EncodeWebSocketFrame(const WebSocketHeader &header, const uint8_t *payload,
                                 size_t size) {
    std::string frame;
    uint64_t len = size;
    bool mask_flag = header._mask_flag && header._mask.size() >= 4;

    frame.push_back((char)(header._fin << 7 | ((header._reserved & 0x07) << 4) |
                           (header._opcode & 0x0F)));
    uint8_t byte = mask_flag ? 0x80 : 0;
    if (len < 126) {
        frame.push_back((char)(byte | len));
    } else if (len <= 0xFFFF) {
        frame.push_back((char)(byte | 126));
        frame.push_back((char)(len >> 8));
        frame.push_back((char)len);
    } else {
        frame.push_back((char)(byte | 127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back((char)(len >> (8 * i)));
        }
    }

    size_t payload_at = frame.size() + (mask_flag ? 4 : 0);
    if (mask_flag) {
        frame.append((const char *)header._mask.data(), 4);
    }
    if (size > 0) {
        frame.append((const char *)payload, size);
    }
    if (mask_flag) {
        for (size_t i = 0; i < size; ++i) {
            frame[payload_at + i] ^= header._mask[i % 4];
        }
    }
    return frame;
}
