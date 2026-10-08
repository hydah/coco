#include "coco/app/ws/conn.hpp"

#include "st.h"

#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/app/http/codec/basic.hpp"
#include "coco/log/log.hpp"

namespace coco {

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

}  // namespace coco
