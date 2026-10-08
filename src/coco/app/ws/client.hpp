#pragma once
#include <stdint.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "coco/base/coroutine_mgr.hpp"
#include "coco/app/http/client.hpp"
#include "coco/net/conn.hpp"
#include "coco/app/ws/conn.hpp"

namespace coco {

#define WS_CLIENT_TIMEOUT_US (int64_t)(3 * 1000 * 1000LL)

// Owns the connection. It is used in one of two ways:
//
//   Like Go's Dial: the caller reads, which also answers PING and CLOSE.
//     WebSocketClient ws;
//     if (ws.Dial("ws://127.0.0.1:9083/echo") == COCO_SUCCESS) {
//         ws.Send("hello");
//         std::string data;
//         ws.ReadMessage(&data);
//     }
//
//   With Start(): a read coroutine hands every message to the message handler.
//
// Send() may be called from any coroutine; once the peer closed, the read side failed or
// Stop() was called, it returns ERROR_WS_CLOSED.
class WebSocketClient {
 public:
    WebSocketClient();
    // Closes the connection (with CLOSE 1000 after Dial, if still open), waits for the
    // read coroutine and for any Send() still writing (bounded by the send timeout), then
    // closes the socket. Must not be called from a message handler or while ReadMessage()
    // runs.
    virtual ~WebSocketClient();

    // wss:// and Start(true, ...) connect with dialer, e.g. TlsDialer() from coco/net/tls;
    // without one they fail with ERROR_HTTPS_NOT_SUPPORTED. ws:// uses TcpDialer().
    void SetTlsDialer(StreamDialer dialer) { tls_dialer_ = dialer; }

    // Extra handshake request headers. Required upgrade headers are set
    // afterwards, so they win if a name collides. Safe to call before Dial.
    void SetHeader(const std::string &key, const std::string &value);

    // Applied now if already connected, and again after the next Dial.
    // kNoTimeout never expires. Until set, reads use the 60s idle default and
    // writes keep Dial's timeout.
    void SetRecvTimeout(int64_t timeout_us);
    void SetSendTimeout(int64_t timeout_us);

    // Connects to url, "ws://host[:port][/path]" or "wss://...", and completes the
    // handshake. timeout_us bounds the connect, the handshake and every write.
    int Dial(const std::string &url, uint64_t timeout_us = WS_CLIENT_TIMEOUT_US);
    // After Dial: blocks until the next data message, see WebSocketConn::ReadMessage().
    int ReadMessage(std::string *data, WebSocketHeader::Type *type = nullptr);

    /**
     * Start
     * 完成WebSocket握手，然后起一条读协程把消息交给 SetMessageHandler 的回调
     * @param host websocket服务器ip或域名
     * @param iPort websocket服务器端口
     * @param timeout_us 连接、握手和每次读写的超时
     */
    int Start(bool is_wss, const std::string &host, uint16_t port, std::string path,
              uint64_t timeout_us = WS_CLIENT_TIMEOUT_US);
    // Stops sending and interrupts the read coroutine without waiting for it.
    int Stop();

    void SetMessageHandler(WebsocketMessageHandler handler) { message_handler_ = handler; }
    int Send(uint8_t *buf, ssize_t len, WebSocketHeader::Type data_type = WebSocketHeader::TEXT);
    int Send(const std::string &data, WebSocketHeader::Type data_type = WebSocketHeader::TEXT) {
        return Send((uint8_t *)data.data(), (ssize_t)data.size(), data_type);
    }
    bool Closed() const { return conn_ == nullptr || conn_->Closed(); }

 private:
    class Reader;

    int Handshake(bool is_wss, const std::string &host, uint16_t port, const std::string &path,
                  uint64_t timeout_us);

    std::string sec_websocket_key_;
    ConnManager *manager_;
    StreamDialer tls_dialer_;
    std::vector<std::pair<std::string, std::string>> extra_headers_;
    bool recv_timeout_set_ = false;
    bool send_timeout_set_ = false;
    int64_t recv_timeout_us_ = 0;
    int64_t send_timeout_us_ = 0;

    // The 101 response: owns the socket and the buffer the connection reads through.
    std::unique_ptr<HttpResponse> upgrade_;
    WebSocketConn *conn_ = nullptr;
    // Deletes itself when its coroutine exits.
    Reader *reader_ = nullptr;
    // set by Start(): the read coroutine owns reading.
    bool background_ = false;
    WebsocketMessageHandler message_handler_ = nullptr;
};


}  // namespace coco
