// WebSocketHandler: the server side of RFC 6455, served through HttpServeMux.

#include <memory>
#include <string>
#include <vector>

#include "coco_api.h"
#include "common/error.hpp"
#include "net/layer4/coco_tcp.hpp"
#include "net/layer7/ws/coco_ws.hpp"
#include "net/layer7/ws/ws_frame.hpp"
#include "net/tls/coco_tls.hpp"
#include "server/coco_http_server.hpp"
#include "server/coco_tcp_server.hpp"
#include "test_util.hpp"

namespace {

typedef WebSocketHeader WS;

const char *kLoopback = "127.0.0.1";
const int kConnectTimeoutUs = 1000 * 1000;
// RFC 6455 1.3's sample key and the accept value it yields.
const char *kKey = "dGhlIHNhbXBsZSBub25jZQ==";
const char *kAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

std::string Frame(WS::Type opcode, const std::string &payload, bool masked = true) {
    WebSocketHeader h;
    h._opcode = opcode;
    h._mask_flag = masked;
    return EncodeWebSocketFrame(h, (const uint8_t *)payload.data(), payload.size());
}

// Header names in lower case and a Connection list, as some clients send them.
std::string UpgradeRequest(const std::string &version = "13", const std::string &key = kKey) {
    return "GET /ws HTTP/1.1\r\n"
           "Host: test\r\n"
           "upgrade: WebSocket\r\n"
           "connection: keep-alive, Upgrade\r\n"
           "sec-websocket-key: " +
           key + "\r\nsec-websocket-version: " + version + "\r\n\r\n";
}

bool WriteAll(TcpConn *c, const std::string &data) {
    return c->Write((void *)data.data(), data.size(), nullptr) == COCO_SUCCESS;
}

bool PeerClosed(TcpConn *c) {
    char b;
    ssize_t n = 0;
    return c->Read(&b, 1, &n) == ERROR_SOCKET_READ && n == 0;
}

// Skips whatever the peer still sends, e.g. the rest of an error body, up to EOF.
bool PeerClosesAfterData(TcpConn *c) {
    char buf[1024];
    ssize_t n = 0;
    int ret;
    while ((ret = c->Read(buf, sizeof(buf), &n)) == COCO_SUCCESS) {
    }
    return ret == ERROR_SOCKET_READ && n == 0;
}

// A hand-written client: reads the upgrade response, then frames sent by the server.
class RawClient {
 public:
    explicit RawClient(int port) {
        if (DialTcp(kLoopback, port, kConnectTimeoutUs, &c_) == COCO_SUCCESS) {
            c_->SetTimeout(1000 * 1000);
        }
    }
    TcpConn *conn() { return c_.get(); }

    // Returns the response header; bytes after it are kept for Next().
    std::string Handshake(const std::string &request) {
        if (!c_ || !WriteAll(c_.get(), request)) {
            return "";
        }
        size_t end;
        while ((end = pending_.find("\r\n\r\n")) == std::string::npos) {
            if (!ReadMore()) {
                return "";
            }
        }
        std::string header = pending_.substr(0, end + 4);
        pending_.erase(0, end + 4);
        return header;
    }

    // The next single-frame message from the server. byte2 gets the frame's second header
    // byte, which holds the mask bit.
    std::unique_ptr<WebSocektMessage> Next(uint8_t *byte2 = nullptr) {
        std::unique_ptr<WebSocektMessage> got;
        while (true) {
            if (pending_.size() >= 2) {
                if (byte2) {
                    *byte2 = (uint8_t)pending_[1];
                }
                WebSocketFrameDecoder d([&got](std::unique_ptr<WebSocektMessage> m) {
                    got = std::move(m);
                    return 1;
                });
                // Decodes at most one frame; a partial one leaves pending_ unchanged.
                std::string copy = pending_;
                d.Decode((const uint8_t *)copy.data(), copy.size());
                if (got) {
                    pending_.erase(0, FrameSize(pending_));
                    return got;
                }
            }
            if (!ReadMore()) {
                return nullptr;
            }
        }
    }

 private:
    static size_t FrameSize(const std::string &b) {
        const uint8_t *p = (const uint8_t *)b.data();
        uint64_t size = p[1] & 0x7F;
        size_t header = 2;
        if (size == 126) {
            size = ((uint64_t)p[2] << 8) | p[3];
            header = 4;
        } else if (size == 127) {
            size = 0;
            for (int i = 0; i < 8; ++i) {
                size = (size << 8) | p[2 + i];
            }
            header = 10;
        }
        return header + ((p[1] & 0x80) ? 4 : 0) + (size_t)size;
    }

    bool ReadMore() {
        char buf[1024];
        ssize_t n = 0;
        if (c_->Read(buf, sizeof(buf), &n) != COCO_SUCCESS) {
            return false;
        }
        pending_.append(buf, n);
        return true;
    }

    std::unique_ptr<TcpConn> c_;
    std::string pending_;
};

bool Is(const WebSocektMessage *m, WS::Type opcode, const std::string &data) {
    return m && m->_opcode == opcode && m->data_ == data;
}

// An echo handler that counts the sessions it started and finished.
struct Events {
    int opened = 0;
    int closed = 0;
    WebSocketConn *last = nullptr;

    WebSocketHandler *Handler() {
        return new WebSocketHandler([this](WebSocketConn *ws) {
            ++opened;
            last = ws;
            std::string data;
            WS::Type type;
            while (ws->ReadMessage(&data, &type) == COCO_SUCCESS) {
                ws->Send(data, type);
            }
            last = nullptr;
            ++closed;
        });
    }
};

}  // namespace

// A WebSocketClient talks to the server: messages are echoed, and the server may push
// from another coroutine through the conn it got in the open handler.
COTEST(WsServerEchoesToClient) {
    const int port = 19221;
    Events ev;
    HttpServeMux mux;
    mux.handle("/ws", ev.Handler());
    HttpServer server(false);
    CHECK_EQ(server.ListenAndServe(kLoopback, port, &mux), COCO_SUCCESS);

    std::vector<std::string> got;
    WebSocketClient ws;
    ws.SetMessageHandler([&got](WebSocketConn *, std::unique_ptr<WebSocektMessage> m) {
        got.push_back(m->data_);
        return 0;
    });
    CHECK_EQ(ws.Start(false, kLoopback, port, "/ws"), COCO_SUCCESS);
    CHECK(cotest::WaitUntil([&]() { return ev.last != nullptr; }));

    std::string msg = "hello";
    CHECK_EQ(ws.Send((uint8_t *)&msg[0], msg.size()), COCO_SUCCESS);
    CHECK(cotest::WaitUntil([&]() { return got.size() == 1; }));

    st_thread_t pusher = cotest::Go([&]() {
        if (ev.last) {
            CHECK_EQ(ev.last->Send("pushed"), COCO_SUCCESS);
        }
    });
    st_thread_join(pusher, NULL);
    CHECK(cotest::WaitUntil([&]() { return got.size() == 2; }));
    CHECK(got.size() == 2 && got[0] == "hello" && got[1] == "pushed");

    // The client's CLOSE is echoed and the server ends the session.
    uint8_t code[2] = {0x03, 0xe8};
    CHECK_EQ(ws.Send(code, sizeof(code), WS::CLOSE), COCO_SUCCESS);
    CHECK(cotest::WaitUntil([&]() { return ev.closed == 1; }));
    CHECK_EQ(ev.opened, 1);
    CHECK(ev.last == nullptr);
}

// Header names in any case, a frame sent right behind the request, and an unmasked echo.
COTEST(WsServerHandshake) {
    const int port = 19222;
    Events ev;
    HttpServeMux mux;
    mux.handle("/ws", ev.Handler());
    HttpServer server(false);
    CHECK_EQ(server.ListenAndServe(kLoopback, port, &mux), COCO_SUCCESS);

    RawClient c(port);
    std::string rsp = c.Handshake(UpgradeRequest() + Frame(WS::TEXT, "early"));
    CHECK(rsp.find("HTTP/1.1 101 ") == 0);
    CHECK(rsp.find("Sec-WebSocket-Accept: " + std::string(kAccept) + "\r\n") !=
          std::string::npos);

    uint8_t byte2 = 0xff;
    std::unique_ptr<WebSocektMessage> m = c.Next(&byte2);
    CHECK(Is(m.get(), WS::TEXT, "early"));
    CHECK_EQ(byte2 & 0x80, 0);
}

// Requests that are not a version 13 upgrade get 400, and the connection is closed.
COTEST(WsServerRejectsBadUpgrade) {
    const int port = 19223;
    Events ev;
    HttpServeMux mux;
    mux.handle("/ws", ev.Handler());
    HttpServer server(false);
    CHECK_EQ(server.ListenAndServe(kLoopback, port, &mux), COCO_SUCCESS);

    {
        RawClient c(port);
        std::string rsp = c.Handshake(UpgradeRequest("8"));
        CHECK(rsp.find("HTTP/1.1 400 ") == 0);
        CHECK(rsp.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos);
        CHECK(c.conn() && PeerClosesAfterData(c.conn()));
    }
    {
        RawClient c(port);
        std::string rsp = c.Handshake(UpgradeRequest("13", "short"));
        CHECK(rsp.find("HTTP/1.1 400 ") == 0);
        CHECK(c.conn() && PeerClosesAfterData(c.conn()));
    }
    {
        // Not an upgrade at all: a plain 400, and keep-alive still applies.
        RawClient c(port);
        std::string rsp = c.Handshake("GET /ws HTTP/1.1\r\nHost: test\r\n\r\n");
        CHECK(rsp.find("HTTP/1.1 400 ") == 0);
    }
    CHECK_EQ(ev.opened, 0);
}

// PING gets an unmasked PONG, CLOSE is echoed with its status code, then the server hangs up.
COTEST(WsServerAnswersPingAndClose) {
    const int port = 19224;
    Events ev;
    HttpServeMux mux;
    mux.handle("/ws", ev.Handler());
    HttpServer server(false);
    CHECK_EQ(server.ListenAndServe(kLoopback, port, &mux), COCO_SUCCESS);

    RawClient c(port);
    CHECK(c.Handshake(UpgradeRequest()).find(" 101 ") != std::string::npos);
    CHECK(WriteAll(c.conn(), Frame(WS::PING, "abc")));
    uint8_t byte2 = 0xff;
    std::unique_ptr<WebSocektMessage> pong = c.Next(&byte2);
    CHECK(Is(pong.get(), WS::PONG, "abc"));
    CHECK_EQ(byte2 & 0x80, 0);

    // The message that shares a packet with the CLOSE is still read and echoed.
    CHECK(WriteAll(c.conn(), Frame(WS::TEXT, "last") +
                                 Frame(WS::CLOSE, std::string("\x03\xe8" "bye", 5))));
    std::unique_ptr<WebSocektMessage> echo = c.Next();
    CHECK(Is(echo.get(), WS::TEXT, "last"));
    std::unique_ptr<WebSocektMessage> reply = c.Next();
    CHECK(Is(reply.get(), WS::CLOSE, std::string("\x03\xe8", 2)));
    CHECK(PeerClosed(c.conn()));
    CHECK(cotest::WaitUntil([&]() { return ev.closed == 1; }));
}

// RFC 6455 5.1: an unmasked client frame is a protocol error, answered with 1002.
COTEST(WsServerRejectsUnmaskedFrame) {
    const int port = 19225;
    Events ev;
    HttpServeMux mux;
    mux.handle("/ws", ev.Handler());
    HttpServer server(false);
    CHECK_EQ(server.ListenAndServe(kLoopback, port, &mux), COCO_SUCCESS);

    RawClient c(port);
    CHECK(c.Handshake(UpgradeRequest()).find(" 101 ") != std::string::npos);
    CHECK(WriteAll(c.conn(), Frame(WS::TEXT, "plain", false)));
    std::unique_ptr<WebSocektMessage> reply = c.Next();
    CHECK(Is(reply.get(), WS::CLOSE, std::string("\x03\xea", 2)));
    CHECK(PeerClosed(c.conn()));
}

// Returning from the handler closes the connection with CLOSE 1000.
COTEST(WsServerHandlerReturnCloses) {
    const int port = 19228;
    HttpServeMux mux;
    mux.handle("/ws", new WebSocketHandler([](WebSocketConn *ws) {
        std::string data;
        if (ws->ReadMessage(&data) == COCO_SUCCESS) {
            ws->Send("bye " + data);
        }
    }));
    HttpServer server(false);
    CHECK_EQ(server.ListenAndServe(kLoopback, port, &mux), COCO_SUCCESS);

    RawClient c(port);
    CHECK(c.Handshake(UpgradeRequest()).find(" 101 ") != std::string::npos);
    CHECK(WriteAll(c.conn(), Frame(WS::TEXT, "tom")));
    std::unique_ptr<WebSocektMessage> m = c.Next();
    CHECK(Is(m.get(), WS::TEXT, "bye tom"));
    std::unique_ptr<WebSocektMessage> close = c.Next();
    CHECK(Is(close.get(), WS::CLOSE, std::string("\x03\xe8", 2)));
    CHECK(PeerClosed(c.conn()));
}

// Stopping the server ends idle sessions: the close handler runs and the client sees EOF.
COTEST(WsServerStopClosesOpenConns) {
    const int port = 19226;
    Events ev;
    HttpServeMux mux;
    mux.handle("/ws", ev.Handler());
    HttpServer *server = new HttpServer(false);
    CHECK_EQ(server->ListenAndServe(kLoopback, port, &mux), COCO_SUCCESS);

    RawClient a(port), b(port);
    CHECK(a.Handshake(UpgradeRequest()).find(" 101 ") != std::string::npos);
    CHECK(b.Handshake(UpgradeRequest()).find(" 101 ") != std::string::npos);
    CHECK(cotest::WaitUntil([&]() { return ev.opened == 2; }));

    delete server;
    CHECK_EQ(ev.closed, 2);
    CHECK(PeerClosed(a.conn()));
    CHECK(PeerClosed(b.conn()));
}

// wss: the same handler behind a TLS TcpServer.
COTEST(WsServerOverTls) {
    const int port = 19227;
    Events ev;
    HttpServeMux mux;
    mux.handle("/ws", ev.Handler());
    TcpServerOptions opt;
    opt.tls_key_file = COCO_SOURCE_DIR "/examples/http-server/server.key";
    opt.tls_crt_file = COCO_SOURCE_DIR "/examples/http-server/server.crt";
    TcpServer server([&mux](StreamConn &conn) { return ServeHttpConn(conn, &mux); }, opt);
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    WebSocketClient ws;
    ws.SetTlsDialer(TlsDialer());
    CHECK_EQ(ws.Dial("wss://127.0.0.1:" + std::to_string(port) + "/ws"), COCO_SUCCESS);
    CHECK_EQ(ws.Send("over tls"), COCO_SUCCESS);
    std::string got;
    CHECK_EQ(ws.ReadMessage(&got), COCO_SUCCESS);
    CHECK(got == "over tls");
}
