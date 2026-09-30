#pragma once
#include <deque>
#include <sstream>
#include "net/layer7/http/coco_http.hpp"
#include "net/layer7/http/http_io.h"
#include "net/layer7/http/http_mux.h"
#include "net/layer7/ws/ws_frame.hpp"
#include "utils/utils.hpp"

#define WS_CLIENT_TIMEOUT_US (int64_t)(3 * 1000 * 1000LL)

class WebSocketConn;
typedef std::function<int(WebSocketConn *, std::unique_ptr<WebSocektMessage> msg)>
    WebsocketMessageHandler;

// One side of an upgraded connection. One coroutine reads with ReadMessage(), which also
// answers PING and CLOSE, so someone must keep reading. Send() may be called from any
// coroutine; once the peer closed, the read side ended or a CLOSE frame went out, it
// returns ERROR_WS_CLOSED.
class WebSocketConn {
 public:
    // conn and reader are not owned; reader yields the bytes that follow the handshake.
    // A client masks what it sends, a server requires what it receives to be masked.
    WebSocketConn(StreamConn *conn, HttpResponseReader *reader, bool is_client);
    // Must not run while a ReadMessage() or Send() does.
    virtual ~WebSocketConn();

    std::string GetRemoteAddr() { return conn_->RemoteAddr(); };

    // Blocks until the next data message, fragments joined. Returns ERROR_WS_CLOSED after
    // the peer's CLOSE, or the error that ended the read side; later calls return the same.
    int ReadMessage(std::string *data, WebSocketHeader::Type *type = nullptr);

    // Writes one frame. Frames never interleave, and nothing is sent after a CLOSE frame,
    // so Send(code, 2, CLOSE) starts the closing handshake.
    int Send(const uint8_t *buf, size_t len,
             WebSocketHeader::Type data_type = WebSocketHeader::TEXT);
    int Send(const std::string &data, WebSocketHeader::Type data_type = WebSocketHeader::TEXT) {
        return Send((const uint8_t *)data.data(), data.size(), data_type);
    }
    bool Closed() const { return closed_; }

 private:
    friend class WebSocketClient;
    friend class WebSocketHandler;

    int ReadNext(std::unique_ptr<WebSocektMessage> *msg);
    // Reads once and decodes what arrived; an error ends the read side.
    int ReadFrames();
    int OnFrame(std::unique_ptr<WebSocektMessage> msg);
    // Hands every message to handler until the read side ends, then Finish().
    int Serve(const WebsocketMessageHandler &handler);
    // Sends CLOSE 1000 if still open, then waits for any Send() still writing (bounded by
    // the send timeout). Afterwards conn is no longer used.
    void Finish();

    StreamConn *conn_;
    HttpResponseReader *reader_;
    bool is_client_;
    WebSocketFrameDecoder decoder_;
    // decoded data messages not read yet.
    std::deque<std::unique_ptr<WebSocektMessage>> inbox_;
    int read_err_ = 0;
    // status code of the peer's CLOSE, echoed once inbox_ is drained.
    std::string peer_close_code_;

    st_mutex_t write_lock_ = nullptr;
    // coroutines inside Send, including those waiting for the lock.
    int writers_ = 0;
    // Set while Serve() waits for writers_ to drop to zero.
    st_cond_t writers_done_ = nullptr;
    bool closed_ = false;
};

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

    // Owns the socket and the upgrade response the connection reads through.
    HttpClient *http_client_ = nullptr;
    WebSocketConn *conn_ = nullptr;
    // Deletes itself when its coroutine exits.
    Reader *reader_ = nullptr;
    // set by Start(): the read coroutine owns reading.
    bool background_ = false;
    WebsocketMessageHandler message_handler_ = nullptr;
};

// Serves WebSocket on the patterns it is registered for in an HttpServeMux, which owns it.
// A request that is not a valid version 13 upgrade gets 400. After the handshake serve
// runs in the HTTP connection's own coroutine; when it returns the connection is closed
// (with CLOSE 1000 if still open) and the conn is freed, so other coroutines may Send()
// on it only until then.
//
//   mux.handle("/echo", new WebSocketHandler([](WebSocketConn *ws) {
//       std::string data;
//       WebSocketHeader::Type type;
//       while (ws->ReadMessage(&data, &type) == COCO_SUCCESS) {
//           ws->Send(data, type);
//       }
//   }));
class WebSocketHandler : public IHttpHandler {
 public:
    typedef std::function<void(WebSocketConn *)> ServeFunc;

    explicit WebSocketHandler(ServeFunc serve) : serve_(serve) {}
    virtual ~WebSocketHandler() = default;

    virtual int serve_http(HttpResponseWriter *w, HttpMessage *r);

 private:
    ServeFunc serve_;
};
