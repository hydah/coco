#pragma once
#include <sstream>
#include "net/layer7/http/coco_http.hpp"
#include "net/layer7/ws/ws_frame.hpp"
#include "utils/utils.hpp"

#define WS_CLIENT_TIMEOUT_US (int64_t)(3 * 1000 * 1000LL)

class WebSocketConn;
typedef std::function<int(WebSocketConn *, std::unique_ptr<WebSocektMessage> msg)>
    WebsocketMessageHandler;

class WebSocketConn : public ConnRoutine {
 public:
    WebSocketConn(void *observer, ConnManager *mgr, StreamConn *conn, HttpMessage *r);
    virtual ~WebSocketConn();
    virtual std::string GetRemoteAddr() { return conn_->RemoteAddr(); };
    int Send(uint8_t *buf, ssize_t len, WebSocketHeader::Type data_type);

 public:
    virtual int DoCycle();

    /**
     * 接收到完整的一个webSocket数据包后回调
     * @param header 数据包包头
     */
    void ProcessMessage(std::unique_ptr<WebSocektMessage> msg);

 private:
    // HttpResponseWriter rsp_writer_ = nullptr;
    HttpMessage *http_msg_ = nullptr;
    StreamConn *conn_ = nullptr;
    WebSocketFrameDecoder decoder_;

    void *observer_ = nullptr;
};

class WebSocketClient {
 public:
    WebSocketClient();
    virtual ~WebSocketClient();

    /**
     * Start
     * 目的是替换TcpClient的连接服务器行为，使之先完成WebSocket握手
     * @param host websocket服务器ip或域名
     * @param iPort websocket服务器端口
     * @param timeout_sec 超时时间
     */
    int Start(bool is_wss, const std::string &host, uint16_t port, std::string path,
              uint64_t timeout_us = WS_CLIENT_TIMEOUT_US);
    // Interrupts the connection without waiting for it to exit.
    int Stop();
    // Called by the connection's destructor; the client drops its pointer to it.
    void OnConnClosed(WebSocketConn *conn);

    void SetMessageHandler(WebsocketMessageHandler handler) { message_handler_ = handler; }
    int HandleMessage(std::unique_ptr<WebSocektMessage> msg);
    int Send(uint8_t *buf, ssize_t len, WebSocketHeader::Type data_type = WebSocketHeader::TEXT);

 private:
    std::string sec_websocket_key_;
    ConnManager *manager_;

    HttpClient *http_client_ = nullptr;

    HttpMessage *ws_http_msg_ = nullptr;
    WebSocketConn *conn_ = nullptr;
    WebsocketMessageHandler message_handler_ = nullptr;
};
