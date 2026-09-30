#include "net/layer7/ws/coco_ws.hpp"
#include "utils/base64.hpp"
#include "utils/sha1.hpp"

WebSocketConn::WebSocketConn(void *observer, ConnManager *mgr, StreamConn *conn, HttpMessage *r)
    : ConnRoutine(mgr),
      decoder_([this](std::unique_ptr<WebSocektMessage> msg) { ProcessMessage(std::move(msg)); }) {
    conn_ = conn;
    http_msg_ = r;
    observer_ = observer;
}

WebSocketConn::~WebSocketConn() {
    coco_info("destruct websocket conn");
    if (observer_) {
        ((WebSocketClient *)observer_)->OnConnClosed(this);
    }
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
            EncodeWebSocketFrameHeader(msg->header_, nullptr, 0);
            // shutdown(SockException(Err_eof, "websocket server close the connection"));
            break;
        }

        case WebSocketHeader::PING: {
            //心跳包
            msg->header_._opcode = WebSocketHeader::PONG;
            EncodeWebSocketFrameHeader(msg->header_, (uint8_t *)msg->data_.data(),
                                       msg->data_.size());
            break;
        }

        case WebSocketHeader::CONTINUATION:
        case WebSocketHeader::TEXT:
        case WebSocketHeader::BINARY: {
            if (!msg->header_._fin) {
                //还有后续分片数据, 我们先缓存数据，所有分片收集完成才一次性输出
                if (msg->data_.size() < MAX_WS_PACKET) {
                    //还有内存容量缓存分片数据
                    decoder_.Continue(std::move(msg), flag);
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

int WebSocketConn::Send(uint8_t *buf, ssize_t len, WebSocketHeader::Type data_type) {
    WebSocketHeader header;
    header._fin = true;
    header._reserved = 0;
    header._opcode = data_type;
    //客户端需要加密
    header._mask_flag = true;

    std::string sh = EncodeWebSocketFrameHeader(header, buf, len);

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
        int nb_read = 0;

        if ((ret = br->Read(buf, HTTP_READ_CACHE_BYTES, &nb_read)) != COCO_SUCCESS) {
            coco_error("read error: %d", ret);
            return ret;
        }

        coco_error("recieved %s", buf);
        decoder_.Decode((uint8_t *)buf, nb_read);
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