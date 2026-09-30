// End-to-end shutdown and peer-close paths through the public server/client classes.

#include <memory>
#include <string>

#include "coco_api.h"
#include "common/error.hpp"
#include "net/layer4/coco_tcp.hpp"
#include "net/layer7/ws/coco_ws.hpp"
#include "server/coco_http_server.hpp"
#include "test_util.hpp"
#include "utils/base64.hpp"
#include "utils/sha1.hpp"

namespace {

const char *kLoopback = "127.0.0.1";
const int kConnectTimeoutUs = 1000 * 1000;

class OkHandler : public IHttpHandler {
 public:
    int serve_http(HttpResponseWriter *w, HttpMessage *r) override {
        std::string body = "ok";
        w->header()->set_content_length((int)body.size());
        return w->Write(const_cast<char *>(body.data()), (int)body.size());
    }
};

// Reads from c until the accumulated data contains marker or the read fails.
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
    ssize_t n = 0;
    return c->Write((void *)data.data(), data.size(), &n) == COCO_SUCCESS;
}

bool PeerClosed(TcpConn *c) {
    c->SetRecvTimeout(1000 * 1000);
    char b;
    ssize_t n = 0;
    return c->Read(&b, 1, &n) == ERROR_SOCKET_READ && n == 0;
}

}  // namespace

// The listen coroutine is parked in st_accept when the server is deleted; deletion must
// return, and the listening port must be released.
COTEST(HttpServerDeletedWhileAccepting) {
    const int port = 19181;
    HttpServeMux mux;
    mux.handle("/", new OkHandler());

    HttpServer *server = new HttpServer(false);
    CHECK_EQ(server->ListenAndServe(kLoopback, port, &mux), 0);
    CocoSleepMs(5);

    delete server;

    std::unique_ptr<TcpConn> refused;
    CHECK(DialTcp(kLoopback, port, kConnectTimeoutUs, &refused) != COCO_SUCCESS);
}

// A keep-alive connection is parked in Parse() waiting for the next request when the
// server is deleted; its coroutine must exit before its socket and parser are freed.
COTEST(HttpServerDeletedWithOpenKeepAliveConn) {
    const int port = 19182;
    HttpServeMux mux;
    mux.handle("/", new OkHandler());

    HttpServer *server = new HttpServer(false);
    CHECK_EQ(server->ListenAndServe(kLoopback, port, &mux), 0);

    std::unique_ptr<TcpConn> client;
    DialTcp(kLoopback, port, kConnectTimeoutUs, &client);
    CHECK(client != nullptr);
    if (!client) {
        delete server;
        return;
    }
    CHECK(WriteAll(client.get(), "GET / HTTP/1.1\r\nHost: test\r\n\r\n"));
    std::string rsp = ReadUntil(client.get(), "\r\n\r\nok");
    CHECK(rsp.find(" 200 ") != std::string::npos);

    delete server;

    CHECK(PeerClosed(client.get()));
}

// The server finishes a request and closes the socket right away; the connection must be
// reclaimed without another accept, which the client sees as EOF.
COTEST(HttpServerClosesNonKeepAliveConn) {
    const int port = 19183;
    HttpServeMux mux;
    mux.handle("/", new OkHandler());

    HttpServer *server = new HttpServer(false);
    CHECK_EQ(server->ListenAndServe(kLoopback, port, &mux), 0);

    std::unique_ptr<TcpConn> client;
    DialTcp(kLoopback, port, kConnectTimeoutUs, &client);
    CHECK(client != nullptr);
    if (client) {
        CHECK(WriteAll(client.get(), "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n"));
        std::string rsp = ReadUntil(client.get(), "\r\n\r\nok");
        CHECK(rsp.find(" 200 ") != std::string::npos);
        CHECK(PeerClosed(client.get()));
    }

    delete server;
}

// The WebSocket server closes right after the handshake. The client's connection coroutine
// ends and frees the connection; later calls on the client must not touch it.
COTEST(WebSocketClientAfterPeerClose) {
    const int port = 19184;
    std::unique_ptr<TcpListener> l;
    CHECK_EQ(ListenTcp(kLoopback, port, &l), COCO_SUCCESS);
    if (!l) {
        return;
    }

    st_thread_t server = cotest::Go([&l]() {
        std::unique_ptr<TcpConn> conn;
        if (l->AcceptTcp(&conn) != COCO_SUCCESS) {
            return;
        }
        TcpConn *c = conn.get();
        std::string req = ReadUntil(c, "\r\n\r\n");
        std::string key_hdr = "Sec-WebSocket-Key: ";
        size_t pos = req.find(key_hdr);
        std::string key;
        if (pos != std::string::npos) {
            size_t start = pos + key_hdr.size();
            key = req.substr(start, req.find("\r\n", start) - start);
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
        CocoSleepMs(20);
    });

    WebSocketClient *ws = new WebSocketClient();
    CHECK_EQ(ws->Start(false, kLoopback, port, "/"), COCO_SUCCESS);
    st_thread_join(server, NULL);
    l.reset();

    // Let the client's connection coroutine observe EOF and exit.
    CocoSleepMs(50);

    std::string msg = "hello";
    CHECK(ws->Send((uint8_t *)msg.data(), msg.size()) != COCO_SUCCESS);
    CHECK_EQ(ws->Stop(), COCO_SUCCESS);
    delete ws;
}
