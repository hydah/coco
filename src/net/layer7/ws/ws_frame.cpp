#include "net/layer7/ws/ws_frame.hpp"

#include <arpa/inet.h>

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

#define CHECK_LEN(size)                                     \
    do {                                                    \
        if (len - (ptr - data) < size) {                    \
            if (cur_msg_->cache_.empty()) {                 \
                cur_msg_->cache_.assign((char *)data, len); \
            }                                               \
            return;                                         \
        }                                                   \
    } while (0)

void WebSocketFrameDecoder::Continue(std::unique_ptr<WebSocektMessage> msg, bool mask_flag) {
    cur_msg_ = std::move(msg);
    cur_msg_->header_.Reset();
    cur_msg_->cache_.clear();
    cur_msg_->got_header_ = false;
    cur_msg_->header_._mask_flag = mask_flag;
}

void WebSocketFrameDecoder::Decode(uint8_t *data, size_t len) {
    if (cur_msg_ == nullptr) {
        cur_msg_.reset(new WebSocektMessage());
    }

    uint8_t *ptr = data;
    if (!cur_msg_->got_header_) {
        //还没有获取数据头
        if (!cur_msg_->cache_.empty()) {
            cur_msg_->cache_.append((char *)data, len);
            data = ptr = (uint8_t *)cur_msg_->cache_.data();
            len = cur_msg_->cache_.size();
        }

        CHECK_LEN(1);
        cur_msg_->header_._fin = (*ptr & 0x80) >> 7;
        cur_msg_->header_._reserved = (*ptr & 0x70) >> 4;
        cur_msg_->header_._opcode = (WebSocketHeader::Type)(*ptr & 0x0F);
        if (!cur_msg_->is_fragmented) {
            cur_msg_->_opcode = cur_msg_->header_._opcode;
            if (cur_msg_->_opcode == WebSocketHeader::CONTINUATION) {
                // error
            }
        }
        ptr += 1;

        CHECK_LEN(1);
        cur_msg_->header_._mask_flag = (*ptr & 0x80) >> 7;
        cur_msg_->header_._payload_len = (*ptr & 0x7F);
        ptr += 1;

        if (cur_msg_->header_._payload_len == 126) {
            CHECK_LEN(2);
            cur_msg_->header_._payload_len = (*ptr << 8) | *(ptr + 1);
            ptr += 2;
        } else if (cur_msg_->header_._payload_len == 127) {
            CHECK_LEN(8);
            cur_msg_->header_._payload_len =
                ((uint64_t)ptr[0] << (8 * 7)) | ((uint64_t)ptr[1] << (8 * 6)) |
                ((uint64_t)ptr[2] << (8 * 5)) | ((uint64_t)ptr[3] << (8 * 4)) |
                ((uint64_t)ptr[4] << (8 * 3)) | ((uint64_t)ptr[5] << (8 * 2)) |
                ((uint64_t)ptr[6] << (8 * 1)) | ((uint64_t)ptr[7] << (8 * 0));
            ptr += 8;
        }
        if (cur_msg_->header_._mask_flag) {
            CHECK_LEN(4);
            cur_msg_->header_._mask.assign(ptr, ptr + 4);
            ptr += 4;
        }
        // 读取到协议头
        cur_msg_->got_header_ = true;

        _mask_offset = 0;
    }

    //进入后面逻辑代表已经获取到了webSocket协议头，
    auto remain = len - (ptr - data);
    if (cur_msg_->header_._payload_len != 0) {
        if (remain > 0) {
            auto copy_len = remain;
            if (remain + cur_msg_->header_.payload_offset_ > cur_msg_->header_._payload_len) {
                copy_len = cur_msg_->header_._payload_len - cur_msg_->data_.size();
            }
            cur_msg_->header_.payload_offset_ += copy_len;

            remain -= copy_len;

            // mask
            if (cur_msg_->header_._mask_flag) {
                for (size_t i = 0; i < copy_len; ++i) {
                    *(ptr + i) ^= cur_msg_->header_._mask[(i + _mask_offset) % 4];
                }
                _mask_offset = (_mask_offset + copy_len) % 4;
            }

            cur_msg_->data_.append((char *)ptr, copy_len);
        }
    }

    // get whole payload
    if (cur_msg_->header_.payload_offset_ == cur_msg_->header_._payload_len) {
        on_frame_(std::move(cur_msg_));

        if (remain > 0 && remain <= len) {
            //解析下一个包
            Decode(data + (len - remain), remain);
        }
    }
}

std::string EncodeWebSocketFrameHeader(WebSocketHeader &header, uint8_t *buffer, uint32_t size) {
    std::string ret;
    uint64_t len = size;
    uint8_t byte = header._fin << 7 | ((header._reserved & 0x07) << 4) | (header._opcode & 0x0F);
    ret.push_back(byte);

    auto mask_flag = (header._mask_flag && header._mask.size() >= 4);
    byte = mask_flag << 7;

    if (len < 126) {
        byte |= len;
        ret.push_back(byte);
    } else if (len <= 0xFFFF) {
        byte |= 126;
        ret.push_back(byte);

        uint16_t len_low = htons((uint16_t)len);
        ret.append((char *)&len_low, 2);
    } else {
        byte |= 127;
        ret.push_back(byte);

        uint32_t len_high = htonl(len >> 32);
        uint32_t len_low = htonl(len & 0xFFFFFFFF);
        ret.append((char *)&len_high, 4);
        ret.append((char *)&len_low, 4);
    }
    if (mask_flag) {
        ret.append((char *)header._mask.data(), 4);
    }

    if (len > 0) {
        if (mask_flag) {
            uint8_t *ptr = buffer;
            for (size_t i = 0; i < len; ++i, ++ptr) {
                *(ptr) ^= header._mask[i % 4];
            }
        }
    }

    return ret;
}
