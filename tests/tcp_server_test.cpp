// TcpServer: one handler call per accepted connection, and a Stop() that waits for them.

#include <memory>
#include <string>

#include "st.h"

#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/net/tcp.hpp"
#include "coco/net/tls/conn.hpp"
#include "coco/net/tcp_server.hpp"
#include "test_util.hpp"

using namespace coco;

namespace {

const char *kLoopback = "127.0.0.1";
const int kConnectTimeoutUs = 1000 * 1000;
const int kClientTimeoutUs = 1000 * 1000;

int Echo(StreamConn &conn) {
    char buf[1024];
    ssize_t n = 0;
    int ret;
    while ((ret = conn.Read(buf, sizeof(buf), &n)) == COCO_SUCCESS) {
        if ((ret = conn.Write(buf, n, nullptr)) != COCO_SUCCESS) {
            break;
        }
    }
    return ret;
}

std::unique_ptr<TcpConn> Dial(int port) {
    std::unique_ptr<TcpConn> c;
    if (DialTcp(kLoopback, port, kConnectTimeoutUs, &c) == COCO_SUCCESS) {
        c->SetTimeout(kClientTimeoutUs);
    }
    return c;
}

std::string ReadN(StreamConn *c, size_t size) {
    std::string data;
    char buf[1024];
    while (data.size() < size) {
        ssize_t n = 0;
        if (c->Read(buf, sizeof(buf), &n) != COCO_SUCCESS) {
            break;
        }
        data.append(buf, n);
    }
    return data;
}

bool WriteAll(StreamConn *c, const std::string &data) {
    return c->Write((void *)data.data(), data.size(), nullptr) == COCO_SUCCESS;
}

bool PeerClosed(TcpConn *c) {
    char b;
    ssize_t n = 0;
    return c->Read(&b, 1, &n) == ERROR_SOCKET_READ && n == 0;
}

}  // namespace

COTEST(TcpServerEchoes) {
    const int port = 19191;
    TcpServer server(Echo);
    CHECK_EQ(server.Start(kLoopback, port), COCO_SUCCESS);

    std::unique_ptr<TcpConn> a(Dial(port));
    std::unique_ptr<TcpConn> b(Dial(port));
    CHECK(a != nullptr && b != nullptr);
    if (!a || !b) {
        return;
    }
    CHECK(WriteAll(a.get(), "ping-a"));
    CHECK(WriteAll(b.get(), "ping-b"));
    CHECK(ReadN(b.get(), 6) == "ping-b");
    CHECK(ReadN(a.get(), 6) == "ping-a");
    CHECK_EQ(server.ConnCount(), 2);
}

// Handlers parked in Read are interrupted; Stop() returns only after every connection
// has been freed, which closes its socket.
COTEST(TcpServerStopClosesOpenConns) {
    const int port = 19192;
    TcpServer server(Echo);
    CHECK_EQ(server.Start(kLoopback, port), COCO_SUCCESS);

    std::unique_ptr<TcpConn> client(Dial(port));
    CHECK(client != nullptr);
    if (!client) {
        return;
    }
    CHECK(cotest::WaitUntil([&]() { return server.ConnCount() == 1; }));

    server.Stop();

    CHECK_EQ(server.ConnCount(), 0);
    CHECK(PeerClosed(client.get()));
}

// Returning from the handler, even with an error, closes that connection only.
COTEST(TcpServerHandlerReturnClosesConn) {
    const int port = 19193;
    int calls = 0;
    TcpServer server([&calls](StreamConn &) {
        ++calls;
        return -1;
    });
    CHECK_EQ(server.Start(kLoopback, port), COCO_SUCCESS);

    for (int i = 0; i < 2; ++i) {
        std::unique_ptr<TcpConn> client(Dial(port));
        CHECK(client != nullptr);
        if (client) {
            CHECK(PeerClosed(client.get()));
        }
    }
    CHECK(cotest::WaitUntil([&]() { return server.ConnCount() == 0; }));
    CHECK_EQ(calls, 2);
}

// The accept coroutine is parked in st_accept; deleting the server must return and
// release the port.
COTEST(TcpServerDeletedWhileAccepting) {
    const int port = 19194;
    TcpServer *server = new TcpServer(Echo);
    CHECK_EQ(server->Start(kLoopback, port), COCO_SUCCESS);
    CocoSleepMs(5);

    delete server;

    std::unique_ptr<TcpConn> refused;
    CHECK(DialTcp(kLoopback, port, kConnectTimeoutUs, &refused) != COCO_SUCCESS);
}

// A handler that never blocks on the connection still learns about Stop().
COTEST(TcpServerHandlerSeesShouldStop) {
    const int port = 19195;
    bool entered = false;
    bool stopped_at_entry = true;
    bool saw_stop = false;
    TcpServer server([&](StreamConn &) {
        entered = true;
        stopped_at_entry = CocoShouldStop();
        while (!CocoShouldStop()) {
            st_usleep(1000);
        }
        saw_stop = true;
        return COCO_SUCCESS;
    });
    CHECK_EQ(server.Start(kLoopback, port), COCO_SUCCESS);

    std::unique_ptr<TcpConn> client(Dial(port));
    CHECK(client != nullptr);
    CHECK(cotest::WaitUntil([&]() { return entered; }));
    CHECK(!CocoShouldStop());

    server.Stop();

    CHECK(!stopped_at_entry);
    CHECK(saw_stop);
}

std::shared_ptr<TlsConfig> ServerTlsConfig() {
    std::shared_ptr<TlsConfig> cfg;
    CHECK_EQ(TlsConfig::NewServer(COCO_SOURCE_DIR "/examples/http-server/server.key",
                                  COCO_SOURCE_DIR "/examples/http-server/server.crt", &cfg),
             COCO_SUCCESS);
    return cfg;
}

// With TlsHandler in front, the handler reads and writes plaintext over TLS.
COTEST(TcpServerServesTls) {
    const int port = 19196;
    TcpServer server(TlsHandler(ServerTlsConfig(), Echo));
    CHECK_EQ(server.Start(kLoopback, port), COCO_SUCCESS);

    std::unique_ptr<TcpConn> tcp = Dial(port);
    CHECK(tcp != nullptr);
    if (!tcp) {
        return;
    }
    std::shared_ptr<TlsConfig> cfg;
    CHECK_EQ(TlsConfig::NewClient(&cfg), COCO_SUCCESS);
    TlsConn tls(std::move(tcp), cfg);
    CHECK_EQ(tls.Handshake(), COCO_SUCCESS);
    CHECK(WriteAll(&tls, "over tls"));
    CHECK(ReadN(&tls, 8) == "over tls");
}

// A peer that does not speak TLS fails the handshake: next is not called, the connection
// is closed and the server keeps accepting.
COTEST(TlsHandlerFailureSkipsNext) {
    const int port = 19185;
    int handled = 0;
    int handshake_ret = -1;
    StreamHandler tls = TlsHandler(ServerTlsConfig(), [&handled](StreamConn &conn) {
        ++handled;
        return Echo(conn);
    });
    TcpServer server([&](StreamConn &conn) { return handshake_ret = tls(conn); });
    CHECK_EQ(server.Start(kLoopback, port), COCO_SUCCESS);

    std::unique_ptr<TcpConn> plain = Dial(port);
    CHECK(plain && WriteAll(plain.get(), "GET / HTTP/1.1\r\n\r\n"));
    CHECK(plain && PeerClosed(plain.get()));
    CHECK_EQ(handshake_ret, ERROR_HTTPS_HANDSHAKE);
    CHECK_EQ(handled, 0);
    CHECK(cotest::WaitUntil([&]() { return server.ConnCount() == 0; }));

    std::unique_ptr<TcpConn> tcp = Dial(port);
    CHECK(tcp != nullptr);
    if (!tcp) {
        return;
    }
    std::shared_ptr<TlsConfig> cfg;
    CHECK_EQ(TlsConfig::NewClient(&cfg), COCO_SUCCESS);
    TlsConn client(std::move(tcp), cfg);
    CHECK_EQ(client.Handshake(), COCO_SUCCESS);
    CHECK(WriteAll(&client, "ok"));
    CHECK(ReadN(&client, 2) == "ok");
    CHECK_EQ(handled, 1);
}

// The server's timeouts are set before the handler runs, so they bound the handshake.
// Without them the handshake would wait for the silent client forever.
COTEST(TlsHandlerTimeoutsBoundHandshake) {
    const int port = 19187;
    int handled = 0;
    int handshake_ret = -1;
    StreamHandler tls = TlsHandler(ServerTlsConfig(), [&handled](StreamConn &) {
        ++handled;
        return COCO_SUCCESS;
    });
    TcpServerOptions opt;
    opt.recv_timeout_us = 100 * 1000;
    TcpServer server([&](StreamConn &conn) { return handshake_ret = tls(conn); }, opt);
    CHECK_EQ(server.Start(kLoopback, port), COCO_SUCCESS);

    std::unique_ptr<TcpConn> silent = Dial(port);
    CHECK(silent != nullptr);
    CHECK(cotest::WaitUntil([&]() { return handshake_ret != -1; }));
    CHECK_EQ(handshake_ret, ERROR_SOCKET_TIMEOUT);
    CHECK(silent && PeerClosed(silent.get()));
    CHECK_EQ(handled, 0);
}

// Stop() interrupts a handshake blocked in a read like any handler, and returns only after
// it has returned and the connection is closed.
COTEST(TlsHandlerStopInterruptsHandshake) {
    const int port = 19188;
    bool entered = false;
    int handled = 0;
    int handshake_ret = -1;
    StreamHandler tls = TlsHandler(ServerTlsConfig(), [&handled](StreamConn &) {
        ++handled;
        return COCO_SUCCESS;
    });
    TcpServer server([&](StreamConn &conn) {
        entered = true;
        return handshake_ret = tls(conn);
    });
    CHECK_EQ(server.Start(kLoopback, port), COCO_SUCCESS);

    std::unique_ptr<TcpConn> silent = Dial(port);
    CHECK(silent != nullptr);
    CHECK(cotest::WaitUntil([&]() { return entered; }));
    CHECK_EQ(handshake_ret, -1);

    server.Stop();

    CHECK_EQ(handshake_ret, ERROR_SOCKET_READ);
    CHECK_EQ(server.ConnCount(), 0);
    CHECK_EQ(handled, 0);
    CHECK(silent && PeerClosed(silent.get()));
}

// An IPv6 literal listens on IPv6, and peer addresses are formatted as [addr]:port.
COTEST(TcpServerIpv6) {
    const int port = 19199;
    std::string remote;
    TcpServer server([&remote](StreamConn &conn) {
        remote = conn.RemoteAddr();
        return Echo(conn);
    });
    CHECK_EQ(server.Start("::1", port), COCO_SUCCESS);

    std::unique_ptr<TcpConn> c;
    CHECK_EQ(DialTcp("::1", port, kConnectTimeoutUs, &c), COCO_SUCCESS);
    if (!c) {
        return;
    }
    c->SetTimeout(kClientTimeoutUs);
    CHECK(WriteAll(c.get(), "v6"));
    CHECK(ReadN(c.get(), 2) == "v6");
    CHECK(remote.compare(0, 6, "[::1]:") == 0);
    CHECK(c->RemoteAddr() == "[::1]:" + std::to_string(port));
}

COTEST(TcpServerServeTwiceFails) {
    TcpServer server(Echo);
    CHECK_EQ(server.Start(kLoopback, 19197), COCO_SUCCESS);
    CHECK(server.Start(kLoopback, 19198) != COCO_SUCCESS);

    std::unique_ptr<TcpConn> refused;
    CHECK(DialTcp(kLoopback, 19198, kConnectTimeoutUs, &refused) != COCO_SUCCESS);
}
