#pragma once
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "coco/base/st_fwd.hpp"
#include "coco/net/conn.hpp"
#include "coco/utils/io.hpp"
#include "coco/app/ws/codec/frame.hpp"

namespace coco {

class WebSocketConn;
typedef std::function<int(WebSocketConn *, std::unique_ptr<WebSocektMessage> msg)>
    WebsocketMessageHandler;

// One side of an upgraded connection. One coroutine reads with ReadMessage(), which also
// answers PING and CLOSE, so someone must keep reading. Send() may be called from any
// coroutine; once the peer closed, the read side ended or a CLOSE frame went out, it
// returns ERROR_WS_CLOSED.
class WebSocketConn {
 public:
    // conn and reader are not owned; reader yields the bytes that follow the handshake,
    // including any that arrived with it. A client masks what it sends, a server requires
    // what it receives to be masked.
    WebSocketConn(StreamConn *conn, IoReader *reader, bool is_client);
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

    // Replaces the timeouts on the underlying stream. The constructor sets the
    // read side to HTTP_RECV_TIMEOUT_US (60s); kNoTimeout waits forever.
    void SetRecvTimeout(int64_t timeout_us) { conn_->SetRecvTimeout(timeout_us); }
    void SetSendTimeout(int64_t timeout_us) { conn_->SetSendTimeout(timeout_us); }

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
    IoReader *reader_;
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

}  // namespace coco
