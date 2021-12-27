#include "net/layer7/coco_ws.hpp"
#include "utils/base64.hpp"
#include "utils/sha1.hpp"

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

WebSocketConn::WebSocketConn(void *observer, ConnManager *mgr, StreamConn *conn, HttpMessage *r)
    : ConnRoutine(mgr) {
    conn_ = conn;
    http_msg_ = r;
    observer_ = observer;
}

WebSocketConn::~WebSocketConn() {
    coco_info("destruct websocket conn");
    coco_freep(conn_);
    coco_freep(http_msg_);
}

/**
 * 接收到完整的一个webSocket数据包后回调
 * @param header 数据包包头
 */
void WebSocketConn::ProcessMessage(std::unique_ptr<WebSocektMessage> msg) {
    if (msg == nullptr) return;

    WebSocketClient *ws_client = (WebSocketClient *)observer_;

    auto flag = msg->header_._mask_flag;
    // websocket客户端发送数据需要加密
    msg->header_._mask_flag = true;

    switch (msg->_opcode) {
        case WebSocketHeader::CLOSE: {
            //服务器主动关闭
            Encode(msg->header_, nullptr, 0);
            // shutdown(SockException(Err_eof, "websocket server close the connection"));
            break;
        }

        case WebSocketHeader::PING: {
            //心跳包
            msg->header_._opcode = WebSocketHeader::PONG;
            Encode(msg->header_, (uint8_t *)msg->data_.data(), msg->data_.size());
            break;
        }

        case WebSocketHeader::CONTINUATION:
        case WebSocketHeader::TEXT:
        case WebSocketHeader::BINARY: {
            if (!msg->header_._fin) {
                //还有后续分片数据, 我们先缓存数据，所有分片收集完成才一次性输出
                if (msg->data_.size() < MAX_WS_PACKET) {
                    //还有内存容量缓存分片数据
                    cur_msg_ = std::move(msg);
                    cur_msg_->header_.Reset();
                    cur_msg_->cache_.clear();
                    cur_msg_->got_header_ = false;
                    cur_msg_->header_._mask_flag = flag;

                    break;
                }
                //分片缓存太大，需要清空
            }

            // get message
            ws_client->HandleMessage(std::move(msg));
            break;
        }

        default:
            break;
    }
}

void WebSocketConn::decode(uint8_t *data, size_t len) {
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
        ProcessMessage(std::move(cur_msg_));

        if (remain > 0 && remain <= len) {
            //解析下一个包
            decode(data + (len - remain), remain);
        }
    }
}

std::string WebSocketConn::Encode(WebSocketHeader &header, uint8_t *buffer, uint32_t size) {
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

int WebSocketConn::Send(uint8_t *buf, ssize_t len, WebSocketHeader::Type data_type) {
    WebSocketHeader header;
    header._fin = true;
    header._reserved = 0;
    header._opcode = data_type;
    //客户端需要加密
    header._mask_flag = true;

    std::string sh = Encode(header, buf, len);

    ssize_t n_write = 0;
    // write header
    conn_->Write((void *)sh.data(), sh.size(), &n_write);
    return conn_->Write(buf, len, &n_write);
}

int WebSocketConn::DoCycle() {
    int ret = COCO_SUCCESS;
    conn_->SetRecvTimeout(HTTP_RECV_TIMEOUT_US);
    HttpResponseReader *br = http_msg_->body_reader();

    // process websocket messages.
    while (!ShouldTermCycle()) {
        char buf[HTTP_READ_CACHE_BYTES];
        ssize_t nb_read = 0;

        if ((ret = conn_->Read(buf, HTTP_READ_CACHE_BYTES, &nb_read)) != COCO_SUCCESS) {
            return ret;
        }

        coco_error("recieved %s", buf);
        decode((uint8_t *)buf, nb_read);
    }

    return ret;
}

WebSocketClient::WebSocketClient() { manager_ = new ConnManager(); }

WebSocketClient::~WebSocketClient() {
    if (manager_) {
        delete manager_;
        manager_ = nullptr;
    }
}

int WebSocketClient::Start(bool is_wss, const std::string &host, uint16_t port, std::string path,
                           uint64_t timeout_us) {
    http_client_ = new HttpClient();

    auto ret = http_client_->Initialize(is_wss, host, port, timeout_us);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    sec_websocket_key_ = base64::Encode((unsigned char *)"1234567890abcdef", 16);
    http_client_->SetMethod("GET");
    http_client_->SetPath(path);

    http_client_->SetHeader("Host", host);
    http_client_->SetHeader("User-Agent", "coco");
    http_client_->SetHeader("Upgrade", "websocket");
    http_client_->SetHeader("Connection", "Upgrade");
    http_client_->SetHeader("Sec-WebSocket-Version", "13");
    http_client_->SetHeader("Sec-WebSocket-Key", sec_websocket_key_);

    if ((ret = http_client_->SendRequest()) != COCO_SUCCESS) {
        return ret;
    }

    ws_http_msg_ = http_client_->GetHttpMessage();
    if (ws_http_msg_ == nullptr) {
        coco_error("http msg is nullptr");
        return -1;
    }

    if (ws_http_msg_->status_code() != 101) {
        coco_error("protocol not swich");
        return -2;
    }

    unsigned char sha[30] = {0};
    std::string src_str = sec_websocket_key_ + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    sha1::calc(src_str.data(), src_str.size(), sha);
    if (base64::Encode((unsigned char *)sha, 20) !=
        ws_http_msg_->get_request_header("Sec-WebSocket-Accept")) {
        // close
        coco_error("Sec-WebSocket-Accept not equal");
        return -3;
    }

    conn_ = new WebSocketConn(this, manager_, http_client_->GetUnderlayerConn(), ws_http_msg_);

    return conn_->Start();
}

int WebSocketClient::HandleMessage(std::unique_ptr<WebSocektMessage> msg) {
    if (message_handler_ != nullptr) {
        message_handler_(conn_, std::move(msg));
    }
}
int WebSocketClient::Send(uint8_t *buf, ssize_t len, WebSocketHeader::Type data_type) {
    if (conn_ == nullptr) return -1;
    return conn_->Send(buf, len, data_type);
}