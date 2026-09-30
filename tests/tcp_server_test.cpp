// TcpServer: one handler call per accepted connection, and a Stop() that waits for them.

#include <memory>
#include <string>

#include "coco_api.h"
#include "common/error.hpp"
#include "net/layer4/coco_tcp.hpp"
#include "net/tls/coco_ssl.hpp"
#include "server/coco_tcp_server.hpp"
#include "test_util.hpp"

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

TcpConn *Dial(int port) {
    TcpConn *c = DialTcp(kLoopback, port, kConnectTimeoutUs);
    if (c) {
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
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

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
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

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
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

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
    CHECK_EQ(server->ListenAndServe(kLoopback, port), COCO_SUCCESS);
    CocoSleepMs(5);

    delete server;

    TcpConn *refused = DialTcp(kLoopback, port, kConnectTimeoutUs);
    CHECK(refused == nullptr);
    delete refused;
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
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    std::unique_ptr<TcpConn> client(Dial(port));
    CHECK(client != nullptr);
    CHECK(cotest::WaitUntil([&]() { return entered; }));
    CHECK(!CocoShouldStop());

    server.Stop();

    CHECK(!stopped_at_entry);
    CHECK(saw_stop);
}

// With a key and certificate, the handler reads and writes plaintext over TLS.
COTEST(TcpServerServesTls) {
    const int port = 19196;
    TcpServerOptions opt;
    opt.tls_key_file = COCO_SOURCE_DIR "/examples/http-server/server.key";
    opt.tls_crt_file = COCO_SOURCE_DIR "/examples/http-server/server.crt";
    TcpServer server(Echo, opt);
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    TcpConn *tcp = Dial(port);
    CHECK(tcp != nullptr);
    if (!tcp) {
        return;
    }
    std::unique_ptr<SslClient> ssl(new SslClient(tcp->GetStfd(), tcp));
    ssl->SetTimeout(kClientTimeoutUs);
    CHECK_EQ(ssl->Handshake(), COCO_SUCCESS);
    CHECK(WriteAll(ssl.get(), "over tls"));
    CHECK(ReadN(ssl.get(), 8) == "over tls");
}

COTEST(TcpServerServeTwiceFails) {
    TcpServer server(Echo);
    CHECK_EQ(server.ListenAndServe(kLoopback, 19197), COCO_SUCCESS);
    CHECK(server.ListenAndServe(kLoopback, 19198) != COCO_SUCCESS);

    TcpConn *refused = DialTcp(kLoopback, 19198, kConnectTimeoutUs);
    CHECK(refused == nullptr);
    delete refused;
}
