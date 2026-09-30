// WebSocket framing (RFC 6455) and the client's replies to control frames.

#include <sys/socket.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "coco_api.h"
#include "common/error.hpp"
#include "net/layer4/coco_tcp.hpp"
#include "net/layer7/ws/coco_ws.hpp"
#include "net/layer7/ws/ws_frame.hpp"
#include "test_util.hpp"
#include "utils/base64.hpp"
#include "utils/sha1.hpp"

namespace {

typedef WebSocketHeader WS;

std::string Frame(WS::Type opcode, const std::string &payload, bool fin = true,
                  bool masked = false) {
    WebSocketHeader h;
    h._opcode = opcode;
    h._fin = fin;
    h._mask_flag = masked;
    return EncodeWebSocketFrame(h, (const uint8_t *)payload.data(), payload.size());
}

struct Collector {
    std::vector<std::unique_ptr<WebSocektMessage>> msgs;
    WebSocketFrameDecoder decoder;

    Collector()
        : decoder([this](std::unique_ptr<WebSocektMessage> m) {
              msgs.push_back(std::move(m));
              return 0;
          }) {}

    int Feed(const std::string &bytes, size_t chunk) {
        for (size_t i = 0; i < bytes.size(); i += chunk) {
            size_t n = std::min(chunk, bytes.size() - i);
            int ret = decoder.Decode((const uint8_t *)bytes.data() + i, n);
            if (ret != 0) {
                return ret;
            }
        }
        return 0;
    }
};

bool Is(const WebSocektMessage *m, WS::Type opcode, const std::string &data) {
    return m && m->_opcode == opcode && m->data_ == data;
}

}  // namespace

COTEST(WsEncodeLengthForms) {
    CHECK(Frame(WS::TEXT, "hello") == std::string("\x81\x05hello"));

    std::string mid = Frame(WS::BINARY, std::string(200, 'x'));
    CHECK(mid.substr(0, 4) == std::string("\x82\x7e\x00\xc8", 4));
    CHECK_EQ(mid.size(), 4 + 200);

    std::string big = Frame(WS::BINARY, std::string(70000, 'x'));
    CHECK(big.substr(0, 10) == std::string("\x82\x7f\x00\x00\x00\x00\x00\x01\x11\x70", 10));
    CHECK_EQ(big.size(), 10 + 70000);
}

// Masking works on the frame's copy; the caller's buffer is left as it was.
COTEST(WsEncodeMasksCopyOnly) {
    std::string payload = "keep me";
    std::string frame = Frame(WS::TEXT, payload, true, true);
    CHECK(payload == "keep me");
    CHECK((frame[1] & 0x80) != 0);
    CHECK(frame.substr(6) != payload);

    Collector c;
    CHECK_EQ(c.Feed(frame, frame.size()), 0);
    CHECK_EQ(c.msgs.size(), 1);
    CHECK(c.msgs.size() == 1 && Is(c.msgs[0].get(), WS::TEXT, "keep me"));

    // Every header draws its own key.
    WebSocketHeader a, b;
    CHECK(a._mask != b._mask);
}

// Frames split at every possible point, including inside a header that is followed by
// another frame in the same read.
COTEST(WsDecodeFramesSplitAnywhere) {
    std::string stream = Frame(WS::TEXT, "first", true, true) + Frame(WS::PING, "") +
                         Frame(WS::BINARY, std::string(300, 'b'), true, true) +
                         Frame(WS::TEXT, "last");
    const size_t chunks[] = {1, 2, 3, 7, 64, stream.size()};
    for (size_t chunk : chunks) {
        Collector c;
        CHECK_EQ(c.Feed(stream, chunk), 0);
        CHECK_EQ(c.msgs.size(), 4);
        if (c.msgs.size() != 4) {
            continue;
        }
        CHECK(Is(c.msgs[0].get(), WS::TEXT, "first"));
        CHECK(Is(c.msgs[1].get(), WS::PING, ""));
        CHECK(Is(c.msgs[2].get(), WS::BINARY, std::string(300, 'b')));
        CHECK(Is(c.msgs[3].get(), WS::TEXT, "last"));
    }
}

// A ping between fragments is delivered alone; the fragments join into one TEXT message.
COTEST(WsDecodeJoinsFragmentsAroundControlFrames) {
    std::string stream = Frame(WS::TEXT, "he", false) + Frame(WS::PING, "p") +
                         Frame(WS::CONTINUATION, "l", false) +
                         Frame(WS::CONTINUATION, "lo", true, true);
    for (size_t chunk = 1; chunk <= stream.size(); ++chunk) {
        Collector c;
        CHECK_EQ(c.Feed(stream, chunk), 0);
        CHECK_EQ(c.msgs.size(), 2);
        if (c.msgs.size() == 2) {
            CHECK(Is(c.msgs[0].get(), WS::PING, "p"));
            CHECK(Is(c.msgs[1].get(), WS::TEXT, "hello"));
        }
    }
}

COTEST(WsDecodeRejectsBadFrames) {
    struct Case {
        const char *name;
        std::string bytes;
        int err;
    };
    std::string rsv1 = Frame(WS::TEXT, "x");
    rsv1[0] |= 0x40;
    // TEXT claiming 2^40 bytes; rejected from the header alone, nothing is buffered.
    std::string huge("\x81\x7f\x00\x00\x01\x00\x00\x00\x00\x00", 10);
    const Case cases[] = {
        {"continuation first", Frame(WS::CONTINUATION, "x"), ERROR_WS_PROTOCOL},
        {"data inside fragments", Frame(WS::TEXT, "a", false) + Frame(WS::TEXT, "b"),
         ERROR_WS_PROTOCOL},
        {"fragmented ping", Frame(WS::PING, "x", false), ERROR_WS_PROTOCOL},
        {"long ping", Frame(WS::PING, std::string(126, 'x')), ERROR_WS_PROTOCOL},
        {"reserved opcode", Frame(WS::RSV3, "x"), ERROR_WS_PROTOCOL},
        {"rsv bit", rsv1, ERROR_WS_PROTOCOL},
        {"huge frame", huge, ERROR_WS_MESSAGE_TOO_LARGE},
        {"huge message", Frame(WS::TEXT, std::string(MAX_WS_PACKET, 'x'), false) +
                             Frame(WS::CONTINUATION, "x"),
         ERROR_WS_MESSAGE_TOO_LARGE},
    };
    for (const Case &k : cases) {
        Collector c;
        int ret = c.Feed(k.bytes, k.bytes.size());
        if (ret != k.err) {
            fprintf(stderr, "case: %s\n", k.name);
        }
        CHECK_EQ(ret, k.err);
        CHECK_EQ(c.msgs.size(), 0);
        // A broken stream stays broken.
        std::string good = Frame(WS::TEXT, "ok");
        CHECK_EQ(c.Feed(good, good.size()), k.err);
    }
}

COTEST(WsDecodeStopsOnHandlerError) {
    int calls = 0;
    WebSocketFrameDecoder d([&calls](std::unique_ptr<WebSocektMessage>) {
        ++calls;
        return 7;
    });
    std::string stream = Frame(WS::TEXT, "a") + Frame(WS::TEXT, "b");
    CHECK_EQ(d.Decode((const uint8_t *)stream.data(), stream.size()), 7);
    CHECK_EQ(calls, 1);
}

namespace {

const char *kLoopback = "127.0.0.1";

std::string ReadUntil(TcpConn *c, const std::string &marker) {
    std::string data;
    char buf[1024];
    while (data.find(marker) == std::string::npos) {
        ssize_t n = 0;
        if (c->Read(buf, sizeof(buf), &n) != COCO_SUCCESS) {
            break;
        }
        data.append(buf, n);
    }
    return data;
}

bool WriteAll(TcpConn *c, const std::string &data) {
    return c->Write((void *)data.data(), data.size(), nullptr) == COCO_SUCCESS;
}

// Accepts one client and completes the upgrade handshake.
TcpConn *AcceptWebSocket(TcpListener *l, std::string *key_out = nullptr) {
    TcpConn *c = l->Accept();
    if (!c) {
        return nullptr;
    }
    c->SetTimeout(1000 * 1000);
    std::string req = ReadUntil(c, "\r\n\r\n");
    std::string key_hdr = "Sec-WebSocket-Key: ";
    std::string key;
    size_t pos = req.find(key_hdr);
    if (pos != std::string::npos) {
        size_t start = pos + key_hdr.size();
        key = req.substr(start, req.find("\r\n", start) - start);
    }
    if (key_out) {
        *key_out = key;
    }
    unsigned char sha[20] = {0};
    std::string src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    sha1::calc(src.data(), src.size(), sha);
    WriteAll(c,
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: " +
                 base64::Encode(sha, sizeof(sha)) + "\r\n\r\n");
    return c;
}

// Reads frames sent by the client until one message is complete. raw gets the bytes.
std::unique_ptr<WebSocektMessage> ReadMessage(TcpConn *c, std::string *raw) {
    std::unique_ptr<WebSocektMessage> got;
    WebSocketFrameDecoder d([&got](std::unique_ptr<WebSocektMessage> m) {
        got = std::move(m);
        return 1;
    });
    char buf[1024];
    while (!got) {
        ssize_t n = 0;
        if (c->Read(buf, sizeof(buf), &n) != COCO_SUCCESS) {
            break;
        }
        raw->append(buf, n);
        d.Decode((const uint8_t *)buf, n);
    }
    return got;
}

bool PeerClosed(TcpConn *c) {
    char b;
    ssize_t n = 0;
    return c->Read(&b, 1, &n) == ERROR_SOCKET_READ && n == 0;
}

}  // namespace

// The client answers a ping with a masked pong carrying the same payload, and its data
// frames arrive intact.
COTEST(WsClientAnswersPing) {
    const int port = 19201;
    TcpListener *l = ListenTcp(kLoopback, port);
    CHECK(l != nullptr);
    if (!l) {
        return;
    }

    bool pong_ok = false, masked = false, text_ok = false;
    st_thread_t server = cotest::Go([&]() {
        std::unique_ptr<TcpConn> c(AcceptWebSocket(l));
        if (!c) {
            return;
        }
        WriteAll(c.get(), Frame(WS::PING, "abc"));
        std::string raw;
        std::unique_ptr<WebSocektMessage> pong = ReadMessage(c.get(), &raw);
        pong_ok = Is(pong.get(), WS::PONG, "abc");
        masked = raw.size() > 1 && (raw[1] & 0x80) != 0;

        raw.clear();
        std::unique_ptr<WebSocektMessage> text = ReadMessage(c.get(), &raw);
        text_ok = Is(text.get(), WS::TEXT, "hello");
    });

    WebSocketClient *ws = new WebSocketClient();
    CHECK_EQ(ws->Start(false, kLoopback, port, "/"), COCO_SUCCESS);
    CocoSleepMs(20);
    std::string msg = "hello";
    CHECK_EQ(ws->Send((uint8_t *)&msg[0], msg.size()), COCO_SUCCESS);
    CHECK(msg == "hello");

    st_thread_join(server, NULL);
    CHECK(pong_ok);
    CHECK(masked);
    CHECK(text_ok);
    delete ws;
    delete l;
}

// A close frame is answered with the same status code, then the client drops the
// connection and refuses to send.
COTEST(WsClientAnswersClose) {
    const int port = 19202;
    TcpListener *l = ListenTcp(kLoopback, port);
    CHECK(l != nullptr);
    if (!l) {
        return;
    }

    bool close_ok = false, closed = false;
    st_thread_t server = cotest::Go([&]() {
        std::unique_ptr<TcpConn> c(AcceptWebSocket(l));
        if (!c) {
            return;
        }
        WriteAll(c.get(), Frame(WS::CLOSE, std::string("\x03\xe8" "bye", 5)));
        std::string raw;
        std::unique_ptr<WebSocektMessage> reply = ReadMessage(c.get(), &raw);
        close_ok = Is(reply.get(), WS::CLOSE, std::string("\x03\xe8", 2));
        closed = PeerClosed(c.get());
    });

    WebSocketClient *ws = new WebSocketClient();
    CHECK_EQ(ws->Start(false, kLoopback, port, "/"), COCO_SUCCESS);
    st_thread_join(server, NULL);
    CHECK(close_ok);
    CHECK(closed);

    std::string msg = "late";
    CHECK_EQ(ws->Send((uint8_t *)&msg[0], msg.size()), ERROR_WS_CLOSED);
    delete ws;
    delete l;
}

// The peer half-closes and stops reading. The read coroutine sees EOF and exits while
// the sender is parked on a full send buffer; the socket must stay open under the sender
// until it times out, and later sends are refused.
COTEST(WsClientSendBlockedWhenReadSideEnds) {
    const int port = 19203;
    TcpListener *l = ListenTcp(kLoopback, port);
    CHECK(l != nullptr);
    if (!l) {
        return;
    }

    bool release = false;
    st_thread_t server = cotest::Go([&]() {
        std::unique_ptr<TcpConn> c(AcceptWebSocket(l));
        if (!c) {
            return;
        }
        shutdown(st_netfd_fileno(c->GetStfd()), SHUT_WR);
        cotest::WaitUntil([&]() { return release; }, 5000);
    });

    WebSocketClient *ws = new WebSocketClient();
    CHECK_EQ(ws->Start(false, kLoopback, port, "/", 300 * 1000), COCO_SUCCESS);
    std::string big(32 * 1024 * 1024, 'x');
    CHECK_EQ(ws->Send((uint8_t *)&big[0], big.size()), ERROR_SOCKET_TIMEOUT);
    CHECK(ws->Closed());
    std::string msg = "late";
    CHECK_EQ(ws->Send((uint8_t *)&msg[0], msg.size()), ERROR_WS_CLOSED);

    release = true;
    st_thread_join(server, NULL);
    delete ws;
    delete l;
}

// Deleting the client while another coroutine is parked in Send waits for that Send to
// give up, instead of freeing the socket and lock under it.
COTEST(WsClientDeletedWhileSending) {
    const int port = 19204;
    TcpListener *l = ListenTcp(kLoopback, port);
    CHECK(l != nullptr);
    if (!l) {
        return;
    }

    bool release = false;
    st_thread_t server = cotest::Go([&]() {
        std::unique_ptr<TcpConn> c(AcceptWebSocket(l));
        cotest::WaitUntil([&]() { return release; }, 5000);
    });

    WebSocketClient *ws = new WebSocketClient();
    CHECK_EQ(ws->Start(false, kLoopback, port, "/", 300 * 1000), COCO_SUCCESS);

    bool sending = false;
    int send_ret = COCO_SUCCESS;
    st_thread_t sender = cotest::Go([&]() {
        std::string big(32 * 1024 * 1024, 'x');
        sending = true;
        send_ret = ws->Send((uint8_t *)&big[0], big.size());
    });
    CHECK(cotest::WaitUntil([&]() { return sending; }));
    CocoSleepMs(20);

    delete ws;
    CHECK_EQ(send_ret, ERROR_SOCKET_TIMEOUT);

    st_thread_join(sender, NULL);
    release = true;
    st_thread_join(server, NULL);
    delete l;
}

// Every connection sends a fresh 16-byte nonce as its key.
COTEST(WsClientKeyIsRandom) {
    const int port = 19205;
    TcpListener *l = ListenTcp(kLoopback, port);
    CHECK(l != nullptr);
    if (!l) {
        return;
    }

    std::string keys[2];
    for (int i = 0; i < 2; ++i) {
        st_thread_t server = cotest::Go([&, i]() {
            std::unique_ptr<TcpConn> c(AcceptWebSocket(l, &keys[i]));
        });
        WebSocketClient ws;
        CHECK_EQ(ws.Start(false, kLoopback, port, "/"), COCO_SUCCESS);
        st_thread_join(server, NULL);
    }
    CHECK_EQ(keys[0].size(), 24);
    CHECK(keys[0] != keys[1]);
    delete l;
}
