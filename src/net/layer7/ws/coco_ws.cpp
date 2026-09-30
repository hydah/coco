#include "net/layer7/ws/coco_ws.hpp"
#include "utils/base64.hpp"
#include "utils/sha1.hpp"

WebSocketConn::WebSocketConn(void *observer, ConnManager *mgr, StreamConn *conn, HttpMessage *r)
    : ConnRoutine(mgr),
      decoder_([this](std::unique_ptr<WebSocektMessage> msg) {
          return ProcessMessage(std::move(msg));
      }) {
    conn_ = conn;
    http_msg_ = r;
    observer_ = observer;
    write_lock_ = st_mutex_new();
}

WebSocketConn::~WebSocketConn() {
    coco_info("destruct websocket conn");
    if (observer_) {
        ((WebSocketClient *)observer_)->OnConnClosed(this);
    }
    coco_freep(conn_);
    coco_freep(http_msg_);
    if (write_lock_) {
        st_mutex_destroy(write_lock_);
        write_lock_ = nullptr;
    }
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
            if (observer_) {
                ((WebSocketClient *)observer_)->HandleMessage(std::move(msg));
            }
            return COCO_SUCCESS;
    }
}

int WebSocketConn::Send(const uint8_t *buf, size_t len, WebSocketHeader::Type data_type) {
    WebSocketHeader header;
    header._opcode = data_type;
    //客户端需要加密
    header._mask_flag = true;
    std::string frame = EncodeWebSocketFrame(header, buf, len);

    if (st_mutex_lock(write_lock_) != 0) {
        return ERROR_THREAD_INTERRUPED;
    }
    int ret = conn_->Write((void *)frame.data(), frame.size(), nullptr);
    st_mutex_unlock(write_lock_);
    return ret;
}

int WebSocketConn::DoCycle() {
    int ret = COCO_SUCCESS;
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

    if ((ret = conn_->Start()) != COCO_SUCCESS) {
        delete conn_;
    }
    return ret;
}

void WebSocketClient::OnConnClosed(WebSocketConn *conn) {
    if (conn_ == conn) {
        conn_ = nullptr;
    }
}

int WebSocketClient::Stop() {
    if (conn_ != nullptr) {
        conn_->Stop();
    }
    return COCO_SUCCESS;
}

int WebSocketClient::HandleMessage(std::unique_ptr<WebSocektMessage> msg) {
    if (message_handler_ != nullptr) {
        return message_handler_(conn_, std::move(msg));
    }
    return COCO_SUCCESS;
}
int WebSocketClient::Send(uint8_t *buf, ssize_t len, WebSocketHeader::Type data_type) {
    if (conn_ == nullptr) return -1;
    return conn_->Send(buf, len, data_type);
}