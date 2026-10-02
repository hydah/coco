#include "coco/net/layer7/ws/coco_ws.hpp"

#include <string.h>
#include <strings.h>

#include <random>

#include "st.h"

#include "http-parser/http_parser.h"

#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/log/log.hpp"
#include "coco/utils/base64.hpp"
#include "coco/utils/sha1.hpp"
#include "coco/utils/utils.hpp"

namespace coco {

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

// RFC 6455 4.2.2: the Sec-WebSocket-Accept value for a Sec-WebSocket-Key.
static std::string WebSocketAccept(const std::string &key) {
    unsigned char sha[20] = {0};
    std::string src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    sha1::calc(src.data(), src.size(), sha);
    return base64::Encode(sha, sizeof(sha));
}

WebSocketConn::WebSocketConn(StreamConn *conn, IoReader *reader, bool is_client)
    : conn_(conn),
      reader_(reader),
      is_client_(is_client),
      decoder_([this](std::unique_ptr<WebSocektMessage> msg) {
          return OnFrame(std::move(msg));
      }, !is_client) {
    write_lock_ = st_mutex_new();
    // An idle connection is normal, so reads wait longer than the connect timeout.
    conn_->SetRecvTimeout(HTTP_RECV_TIMEOUT_US);
}

WebSocketConn::~WebSocketConn() {
    if (write_lock_) {
        st_mutex_destroy(write_lock_);
        write_lock_ = nullptr;
    }
}

int WebSocketConn::OnFrame(std::unique_ptr<WebSocektMessage> msg) {
    switch (msg->_opcode) {
        case WebSocketHeader::CLOSE:
            peer_close_code_ = msg->data_.substr(0, msg->data_.size() >= 2 ? 2 : 0);
            return ERROR_WS_CLOSED;

        case WebSocketHeader::PING:
            //心跳包
            return Send((const uint8_t *)msg->data_.data(), msg->data_.size(),
                        WebSocketHeader::PONG);

        case WebSocketHeader::PONG:
            return COCO_SUCCESS;

        default:
            inbox_.push_back(std::move(msg));
            return COCO_SUCCESS;
    }
}

int WebSocketConn::ReadFrames() {
    if (CocoShouldStop()) {
        return ERROR_THREAD_INTERRUPED;
    }

    char buf[HTTP_READ_CACHE_BYTES];
    ssize_t nb_read = 0;
    int ret = reader_->Read(buf, HTTP_READ_CACHE_BYTES, &nb_read);
    if (ret != COCO_SUCCESS) {
        if (!coco_is_client_gracefully_close(ret)) {
            coco_error("websocket read error. ret=%d", ret);
        }
        return ret;
    }

    ret = decoder_.Decode((uint8_t *)buf, nb_read);
    if (ret == ERROR_WS_PROTOCOL || ret == ERROR_WS_MESSAGE_TOO_LARGE) {
        // RFC 6455 7.4.1: 1002 protocol error, 1009 message too big.
        uint16_t code = ret == ERROR_WS_PROTOCOL ? 1002 : 1009;
        uint8_t payload[2] = {(uint8_t)(code >> 8), (uint8_t)code};
        Send(payload, sizeof(payload), WebSocketHeader::CLOSE);
        coco_error("websocket: bad frame from peer. ret=%d", ret);
    }
    return ret;
}

int WebSocketConn::ReadNext(std::unique_ptr<WebSocektMessage> *msg) {
    // Messages decoded ahead of a CLOSE or a bad frame are still delivered.
    while (inbox_.empty()) {
        if (read_err_ != COCO_SUCCESS) {
            // RFC 6455 5.5.1: echo the peer's status code once the messages before its
            // CLOSE were read, so replies to them still go out first.
            if (read_err_ == ERROR_WS_CLOSED && !closed_) {
                Send(peer_close_code_, WebSocketHeader::CLOSE);
            }
            closed_ = true;
            return read_err_;
        }
        read_err_ = ReadFrames();
        if (read_err_ != COCO_SUCCESS && read_err_ != ERROR_WS_CLOSED) {
            closed_ = true;
        }
    }
    *msg = std::move(inbox_.front());
    inbox_.pop_front();
    return COCO_SUCCESS;
}

int WebSocketConn::ReadMessage(std::string *data, WebSocketHeader::Type *type) {
    std::unique_ptr<WebSocektMessage> msg;
    int ret = ReadNext(&msg);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    data->swap(msg->data_);
    if (type) {
        *type = msg->_opcode;
    }
    return COCO_SUCCESS;
}

int WebSocketConn::Serve(const WebsocketMessageHandler &handler) {
    std::unique_ptr<WebSocektMessage> msg;
    int ret;
    while ((ret = ReadNext(&msg)) == COCO_SUCCESS) {
        if (handler != nullptr) {
            handler(this, std::move(msg));
        }
    }
    Finish();
    return ret == ERROR_WS_CLOSED ? COCO_SUCCESS : ret;
}

void WebSocketConn::Finish() {
    if (!closed_) {
        uint8_t normal[2] = {0x03, 0xe8};
        Send(normal, sizeof(normal), WebSocketHeader::CLOSE);
    }
    closed_ = true;

    // A Send parked in a write returns once it times out; conn must stay open under it.
    // The check and the wait do not yield, so the last writer's signal is not lost.
    if (writers_ > 0) {
        writers_done_ = st_cond_new();
        while (writers_ > 0) {
            st_cond_wait(writers_done_);
        }
        st_cond_destroy(writers_done_);
        writers_done_ = nullptr;
    }
}

int WebSocketConn::Send(const uint8_t *buf, size_t len, WebSocketHeader::Type data_type) {
    if (closed_) {
        return ERROR_WS_CLOSED;
    }

    WebSocketHeader header;
    header._opcode = data_type;
    // RFC 6455 5.1: clients mask every frame, servers never do.
    header._mask_flag = is_client_;
    std::string frame = EncodeWebSocketFrame(header, buf, len);

    int ret = ERROR_THREAD_INTERRUPED;
    ++writers_;
    if (st_mutex_lock(write_lock_) == 0) {
        // The connection may have closed, or sent CLOSE, while this writer waited its turn.
        ret = ERROR_WS_CLOSED;
        if (!closed_) {
            ret = conn_->Write((void *)frame.data(), frame.size(), nullptr);
            if (data_type == WebSocketHeader::CLOSE) {
                closed_ = true;
            }
        }
        st_mutex_unlock(write_lock_);
    }
    --writers_;

    if (writers_ == 0 && writers_done_) {
        st_cond_signal(writers_done_);
    }
    return ret;
}

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
    http_parser_url u;
    memset(&u, 0, sizeof(u));
    if (http_parser_parse_url(url.data(), url.size(), 0, &u) != 0) {
        coco_error("websocket: bad url %s", url.c_str());
        return ERROR_HTTP_PARSE_URI;
    }
    auto field = [&](http_parser_url_fields f) {
        if ((u.field_set & (1 << f)) == 0) {
            return std::string();
        }
        return url.substr(u.field_data[f].off, u.field_data[f].len);
    };

    std::string schema = field(UF_SCHEMA);
    bool is_wss = strcasecmp(schema.c_str(), "wss") == 0;
    std::string host = field(UF_HOST);
    if ((!is_wss && strcasecmp(schema.c_str(), "ws") != 0) || host.empty()) {
        coco_error("websocket: url must be ws://host or wss://host, got %s", url.c_str());
        return ERROR_HTTP_PARSE_URI;
    }
    uint16_t port = u.port != 0 ? u.port : (is_wss ? 443 : 80);
    std::string path = field(UF_PATH);
    if (path.empty()) {
        path = "/";
    }
    if (u.field_set & (1 << UF_QUERY)) {
        path += "?" + field(UF_QUERY);
    }
    return Handshake(is_wss, host, port, path, timeout_us);
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
    sec_websocket_key_ = NewWebSocketKey();
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
    if (WebSocketAccept(sec_websocket_key_) != resp->header.Get("Sec-WebSocket-Accept")) {
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

void WebSocketHandler::ServeHTTP(HttpResponseWriter &w, HttpRequest &r) {
    // RFC 6455 4.2.1. IsUpgrade() covers the Connection: upgrade token.
    const std::string &key = r.header.Get("Sec-WebSocket-Key");
    if (r.method != HttpMethodGet || !r.IsUpgrade() ||
        !r.header.HasToken(HttpHeaderUpgrade, "websocket") || base64::decode(key).size() != 16) {
        HttpError(w, HttpStatusText(HttpStatusBadRequest), HttpStatusBadRequest);
        return;
    }
    if (r.header.Get("Sec-WebSocket-Version") != "13") {
        w.Header().Set("Sec-WebSocket-Version", "13");
        HttpError(w, HttpStatusText(HttpStatusBadRequest), HttpStatusBadRequest);
        return;
    }

    StreamConn *conn = nullptr;
    BufReader *br = nullptr;
    if (w.Hijack(&conn, &br) != COCO_SUCCESS) {
        return;
    }
    std::string rsp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        WebSocketAccept(key) + "\r\n\r\n";
    if (conn->Write((void *)rsp.data(), rsp.size(), nullptr) != COCO_SUCCESS) {
        return;
    }

    WebSocketConn ws(conn, br, false);
    serve_(&ws);
    // Read errors were logged, and the connection ends here either way.
    ws.Finish();
}

}  // namespace coco
