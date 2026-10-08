#include "coco/app/ws/client.hpp"

#include "coco/base/coroutine.hpp"
#include "coco/common/error.hpp"
#include "coco/log/log.hpp"
#include "coco/utils/utils.hpp"
#include "coco/app/ws/codec/handshake.hpp"

namespace coco {

// Runs the client's connection, then closes the socket.
class WebSocketClient::Reader : public ConnRoutine {
 public:
    explicit Reader(WebSocketClient *client) : ConnRoutine(client->manager_), client_(client) {}
    virtual ~Reader() { client_->reader_ = nullptr; }

    virtual std::string GetRemoteAddr() { return client_->conn_->GetRemoteAddr(); }

 protected:
    virtual int DoCycle() {
        int ret = client_->conn_->Serve(client_->message_handler_);
        client_->upgrade_->Close();
        return ret;
    }

 private:
    WebSocketClient *client_;
};

WebSocketClient::WebSocketClient() { manager_ = new ConnManager(); }

WebSocketClient::~WebSocketClient() {
    if (conn_ != nullptr) {
        if (background_) {
            conn_->closed_ = true;
        } else {
            // Nobody reads after Dial, so the connection is closed here.
            conn_->Finish();
        }
    }
    // Interrupts the read coroutine and waits until it has released the socket.
    if (manager_) {
        delete manager_;
        manager_ = nullptr;
    }
    coco_freep(conn_);
    upgrade_.reset();
}

int WebSocketClient::Dial(const std::string &url, uint64_t timeout_us) {
    WebSocketUrl u;
    int ret = ParseWebSocketUrl(url, &u);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    return Handshake(u.tls, u.host, u.port, u.path, timeout_us);
}

int WebSocketClient::ReadMessage(std::string *data, WebSocketHeader::Type *type) {
    if (background_) {
        coco_error("websocket: ReadMessage after Start, the read coroutine owns reading");
        return ERROR_THREAD_STARTED;
    }
    if (conn_ == nullptr) {
        return ERROR_WS_CLOSED;
    }
    return conn_->ReadMessage(data, type);
}

int WebSocketClient::Start(bool is_wss, const std::string &host, uint16_t port, std::string path,
                           uint64_t timeout_us) {
    int ret = Handshake(is_wss, host, port, path, timeout_us);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    background_ = true;
    reader_ = new Reader(this);
    if ((ret = reader_->Start()) != COCO_SUCCESS) {
        delete reader_;
        conn_->closed_ = true;
        upgrade_->Close();
    }
    return ret;
}

int WebSocketClient::Handshake(bool is_wss, const std::string &host, uint16_t port,
                               const std::string &path, uint64_t timeout_us) {
    if (upgrade_ != nullptr) {
        coco_error("websocket client already started");
        return ERROR_THREAD_STARTED;
    }
    if (is_wss && !tls_dialer_) {
        coco_error("websocket: wss needs SetTlsDialer()");
        return ERROR_HTTPS_NOT_SUPPORTED;
    }

    HttpClient client((int64_t)timeout_us);
    client.SetTlsDialer(tls_dialer_);
    client.max_redirects = 0;

    std::string host_part = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    HttpRequest req(HttpMethodGet, std::string(is_wss ? "https://" : "http://") + host_part + ":" +
                               std::to_string(port) + path);
    for (size_t i = 0; i < extra_headers_.size(); i++) {
        req.header.Set(extra_headers_[i].first, extra_headers_[i].second);
    }
    sec_websocket_key_ = WebSocketNewKey();
    req.header.Set(HttpHeaderUpgrade, "websocket");
    req.header.Set(HttpHeaderConnection, "Upgrade");
    req.header.Set("Sec-WebSocket-Version", "13");
    req.header.Set("Sec-WebSocket-Key", sec_websocket_key_);

    std::unique_ptr<HttpResponse> resp;
    int ret = client.Do(req, &resp);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    if (resp->status_code != HttpStatusSwitchingProtocols) {
        coco_error("websocket: handshake answered %s", resp->status.c_str());
        return ERROR_HTTP_STATUS_INVALID;
    }
    if (WebSocketAcceptKey(sec_websocket_key_) != resp->header.Get("Sec-WebSocket-Accept")) {
        coco_error("websocket: Sec-WebSocket-Accept does not match the key");
        return ERROR_HTTP_STATUS_INVALID;
    }

    upgrade_ = std::move(resp);
    conn_ = new WebSocketConn(upgrade_->Conn(), upgrade_->Reader(), true);
    if (recv_timeout_set_) {
        conn_->SetRecvTimeout(recv_timeout_us_);
    }
    if (send_timeout_set_) {
        conn_->SetSendTimeout(send_timeout_us_);
    }
    return COCO_SUCCESS;
}

void WebSocketClient::SetHeader(const std::string& key, const std::string& value) {
    for (size_t i = 0; i < extra_headers_.size(); i++) {
        if (extra_headers_[i].first == key) {
            extra_headers_[i].second = value;
            return;
        }
    }
    extra_headers_.push_back(std::make_pair(key, value));
}

void WebSocketClient::SetRecvTimeout(int64_t timeout_us) {
    recv_timeout_set_ = true;
    recv_timeout_us_ = timeout_us;
    if (conn_ != nullptr) {
        conn_->SetRecvTimeout(timeout_us);
    }
}

void WebSocketClient::SetSendTimeout(int64_t timeout_us) {
    send_timeout_set_ = true;
    send_timeout_us_ = timeout_us;
    if (conn_ != nullptr) {
        conn_->SetSendTimeout(timeout_us);
    }
}

int WebSocketClient::Stop() {
    if (conn_ != nullptr) {
        conn_->closed_ = true;
    }
    if (reader_ != nullptr) {
        reader_->Stop();
    }
    return COCO_SUCCESS;
}

int WebSocketClient::Send(uint8_t *buf, ssize_t len, WebSocketHeader::Type data_type) {
    if (conn_ == nullptr) {
        return ERROR_WS_CLOSED;
    }
    return conn_->Send(buf, (size_t)len, data_type);
}

}  // namespace coco
