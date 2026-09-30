#pragma once
#include <sstream>
#include "net/layer7/http/coco_http.hpp"
#include "net/layer7/ws/ws_frame.hpp"
#include "utils/utils.hpp"

#define WS_CLIENT_TIMEOUT_US (int64_t)(3 * 1000 * 1000LL)

class WebSocketClient;
class WebSocketConn;
typedef std::function<int(WebSocketConn *, std::unique_ptr<WebSocektMessage> msg)>
    WebsocketMessageHandler;

// The reading side of a client connection: its coroutine decodes frames, answers PING
// and CLOSE, and hands data messages to the client. It frees itself when it exits but
// never the socket, which belongs to the WebSocketClient.
class WebSocketConn : public ConnRoutine {
 public:
    WebSocketConn(WebSocketClient *client, ConnManager *mgr, StreamConn *conn, HttpMessage *r);
    virtual ~WebSocketConn();
    virtual std::string GetRemoteAddr() { return conn_->RemoteAddr(); };
    // Same as WebSocketClient::Send; lets a message handler reply.
    int Send(const uint8_t *buf, size_t len, WebSocketHeader::Type data_type);

 public:
    virtual int DoCycle();

    /**
     * 接收到完整的一个webSocket数据包后回调
     * 回复 PING / CLOSE，数据消息交给 WebSocketClient
     */
    int ProcessMessage(std::unique_ptr<WebSocektMessage> msg);

 private:
    WebSocketClient *client_;
    // Not owned: both belong to the client's HttpClient.
    StreamConn *conn_;
    HttpMessage *http_msg_;
    WebSocketFrameDecoder decoder_;
};

// Owns the connection. Send() may be called from any coroutine; once the peer closed,
// the read side failed or Stop() was called, it returns ERROR_WS_CLOSED. The socket is
// closed when the read coroutine has exited and no Send() is still writing to it. The
// client must not be deleted while another coroutine is inside Send().
class WebSocketClient {
 public:
    WebSocketClient();
    // Stops the read coroutine and waits for it, then closes the socket.
    virtual ~WebSocketClient();

    /**
     * Start
     * 目的是替换TcpClient的连接服务器行为，使之先完成WebSocket握手
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
    bool Closed() const { return closed_; }

 private:
    friend class WebSocketConn;

    // Writes one masked frame. Frames never interleave, and nothing is sent after a
    // CLOSE frame went out.
    int WriteFrame(const uint8_t *buf, size_t len, WebSocketHeader::Type data_type);
    int HandleMessage(WebSocketConn *conn, std::unique_ptr<WebSocektMessage> msg);
    // Called by the read coroutine's destructor.
    void OnConnClosed();
    // Closes the socket once neither the read coroutine nor a writer can touch it.
    void CloseSocketIfIdle();

    std::string sec_websocket_key_;
    ConnManager *manager_;

    // Owns the socket and the upgrade response the read coroutine reads through.
    HttpClient *http_client_ = nullptr;
    StreamConn *stream_ = nullptr;
    st_mutex_t write_lock_ = nullptr;
    // coroutines inside WriteFrame, including those waiting for the lock.
    int writers_ = 0;
    bool closed_ = true;

    WebSocketConn *conn_ = nullptr;
    WebsocketMessageHandler message_handler_ = nullptr;
};
