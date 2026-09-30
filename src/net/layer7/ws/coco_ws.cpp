#include "net/layer7/ws/coco_ws.hpp"

#include <string.h>

#include <random>

#include "utils/base64.hpp"
#include "utils/sha1.hpp"

// RFC 6455 4.1: a random 16-byte nonce, base64 encoded, chosen anew for each connection.
static std::string NewWebSocketKey() {
    static std::random_device rd;
    unsigned char nonce[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t v = rd();
        memcpy(nonce + i, &v, 4);
    }
    return base64::Encode(nonce, sizeof(nonce));
}

WebSocketConn::WebSocketConn(WebSocketClient *client, ConnManager *mgr, StreamConn *conn,
                             HttpMessage *r)
    : ConnRoutine(mgr),
      client_(client),
      conn_(conn),
      http_msg_(r),
      decoder_([this](std::unique_ptr<WebSocektMessage> msg) {
          return ProcessMessage(std::move(msg));
      }) {}

WebSocketConn::~WebSocketConn() {
    coco_info("destruct websocket conn");
    client_->OnConnClosed();
}

int WebSocketConn::Send(const uint8_t *buf, size_t len, WebSocketHeader::Type data_type) {
    return client_->WriteFrame(buf, len, data_type);
}

int WebSocketConn::ProcessMessage(std::unique_ptr<WebSocektMessage> msg) {
    switch (msg->_opcode) {
        case WebSocketHeader::CLOSE: {
            // RFC 6455 5.5.1: answer with a close frame echoing the status code, then stop.
            size_t code_len = msg->data_.size() >= 2 ? 2 : 0;
            Send((const uint8_t *)msg->data_.data(), code_len, WebSocketHeader::CLOSE);
            return ERROR_WS_CLOSED;
        }

        case WebSocketHeader::PING:
            //心跳包
            return Send((const uint8_t *)msg->data_.data(), msg->data_.size(),
                        WebSocketHeader::PONG);

        case WebSocketHeader::PONG:
            return COCO_SUCCESS;

        default:
            client_->HandleMessage(this, std::move(msg));
            return COCO_SUCCESS;
    }
}

int WebSocketConn::DoCycle() {
    int ret = COCO_SUCCESS;
    // An idle connection is normal, so reads wait longer than the connect timeout.
    conn_->SetRecvTimeout(HTTP_RECV_TIMEOUT_US);
    HttpResponseReader *br = http_msg_->body_reader();

    // process websocket messages.
    while (!ShouldTermCycle()) {
        char buf[HTTP_READ_CACHE_BYTES];
        int nb_read = 0;

        if ((ret = br->Read(buf, HTTP_READ_CACHE_BYTES, &nb_read)) != COCO_SUCCESS) {
            if (!coco_is_client_gracefully_close(ret)) {
                coco_error("websocket read error. ret=%d", ret);
            }
            return ret;
        }

        if ((ret = decoder_.Decode((uint8_t *)buf, nb_read)) != COCO_SUCCESS) {
            break;
        }
    }

    if (ret == ERROR_WS_CLOSED) {
        return COCO_SUCCESS;
    }
    if (ret == ERROR_WS_PROTOCOL || ret == ERROR_WS_MESSAGE_TOO_LARGE) {
        // RFC 6455 7.4.1: 1002 protocol error, 1009 message too big.
        uint16_t code = ret == ERROR_WS_PROTOCOL ? 1002 : 1009;
        uint8_t payload[2] = {(uint8_t)(code >> 8), (uint8_t)code};
        Send(payload, sizeof(payload), WebSocketHeader::CLOSE);
        coco_error("websocket: bad frame from peer. ret=%d", ret);
    }
    return ret;
}

WebSocketClient::WebSocketClient() {
    manager_ = new ConnManager();
    write_lock_ = st_mutex_new();
}

WebSocketClient::~WebSocketClient() {
    // The read coroutine uses the socket and the upgrade response until it has exited.
    closed_ = true;
    if (manager_) {
        delete manager_;
        manager_ = nullptr;
    }
    // A Send parked in a write returns once it times out; it must not wake up on a freed
    // client. The check and the wait do not yield, so the last writer's signal is not lost.
    if (writers_ > 0) {
        writers_done_ = st_cond_new();
        while (writers_ > 0) {
            st_cond_wait(writers_done_);
        }
        st_cond_destroy(writers_done_);
        writers_done_ = nullptr;
    }
    coco_freep(http_client_);
    stream_ = nullptr;
    if (write_lock_) {
        st_mutex_destroy(write_lock_);
        write_lock_ = nullptr;
    }
}

int WebSocketClient::Start(bool is_wss, const std::string &host, uint16_t port, std::string path,
                           uint64_t timeout_us) {
    if (http_client_ != nullptr) {
        coco_error("websocket client already started");
        return ERROR_THREAD_STARTED;
    }
    http_client_ = new HttpClient();

    auto ret = http_client_->Initialize(is_wss, host, port, timeout_us);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    sec_websocket_key_ = NewWebSocketKey();
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

    HttpMessage *ws_http_msg = http_client_->GetHttpMessage();
    if (ws_http_msg == nullptr) {
        coco_error("http msg is nullptr");
        return -1;
    }

    if (ws_http_msg->status_code() != 101) {
        coco_error("protocol not swich");
        return -2;
    }

    unsigned char sha[30] = {0};
    std::string src_str = sec_websocket_key_ + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    sha1::calc(src_str.data(), src_str.size(), sha);
    if (base64::Encode((unsigned char *)sha, 20) !=
        ws_http_msg->get_request_header("Sec-WebSocket-Accept")) {
        // close
        coco_error("Sec-WebSocket-Accept not equal");
        return -3;
    }

    stream_ = http_client_->GetUnderlayerConn();
    conn_ = new WebSocketConn(this, manager_, stream_, ws_http_msg);
    closed_ = false;
    if ((ret = conn_->Start()) != COCO_SUCCESS) {
        closed_ = true;
        delete conn_;
        conn_ = nullptr;
    }
    return ret;
}

void WebSocketClient::OnConnClosed() {
    conn_ = nullptr;
    closed_ = true;
    CloseSocketIfIdle();
}

void WebSocketClient::CloseSocketIfIdle() {
    if (conn_ != nullptr || writers_ > 0 || stream_ == nullptr) {
        return;
    }
    stream_ = nullptr;
    http_client_->Disconnect();
}

int WebSocketClient::Stop() {
    closed_ = true;
    if (conn_ != nullptr) {
        conn_->Stop();
    }
    return COCO_SUCCESS;
}

int WebSocketClient::HandleMessage(WebSocketConn *conn, std::unique_ptr<WebSocektMessage> msg) {
    if (message_handler_ != nullptr) {
        return message_handler_(conn, std::move(msg));
    }
    return COCO_SUCCESS;
}

int WebSocketClient::Send(uint8_t *buf, ssize_t len, WebSocketHeader::Type data_type) {
    return WriteFrame(buf, (size_t)len, data_type);
}

int WebSocketClient::WriteFrame(const uint8_t *buf, size_t len, WebSocketHeader::Type data_type) {
    if (closed_) {
        return ERROR_WS_CLOSED;
    }

    WebSocketHeader header;
    header._opcode = data_type;
    //客户端需要加密
    header._mask_flag = true;
    std::string frame = EncodeWebSocketFrame(header, buf, len);

    int ret = ERROR_THREAD_INTERRUPED;
    ++writers_;
    if (st_mutex_lock(write_lock_) == 0) {
        // The connection may have closed, or sent CLOSE, while this writer waited its turn.
        ret = ERROR_WS_CLOSED;
        if (!closed_) {
            ret = stream_->Write((void *)frame.data(), frame.size(), nullptr);
            if (data_type == WebSocketHeader::CLOSE) {
                closed_ = true;
            }
        }
        st_mutex_unlock(write_lock_);
    }
    --writers_;

    if (closed_) {
        CloseSocketIfIdle();
    }
    if (writers_ == 0 && writers_done_) {
        st_cond_signal(writers_done_);
    }
    return ret;
}
