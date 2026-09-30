// TlsConn: plaintext ReadFully, writers sharing one connection, and TLS over any StreamConn.

#include <memory>
#include <string>

#include "coco_api.h"
#include "common/error.hpp"
#include "net/layer4/coco_tcp.hpp"
#include "net/tls/coco_tls.hpp"
#include "server/coco_tcp_server.hpp"
#include "test_util.hpp"

namespace {

const char *kLoopback = "127.0.0.1";
const int kTimeoutUs = 2000 * 1000;

// Forwards to another StreamConn and counts the bytes that pass.
class CountingConn : public StreamConn {
 public:
    explicit CountingConn(std::unique_ptr<StreamConn> under) : under_(std::move(under)) {}

    int Read(void *buf, size_t size, ssize_t *nread) override {
        ssize_t n = 0;
        int ret = under_->Read(buf, size, &n);
        read_bytes += n > 0 ? n : 0;
        if (nread) {
            *nread = n;
        }
        return ret;
    }
    int Write(void *buf, size_t size, ssize_t *nwrite) override {
        written_bytes += size;
        return under_->Write(buf, size, nwrite);
    }
    int Writev(const iovec *iov, int iov_size, ssize_t *nwrite) override {
        for (int i = 0; i < iov_size; ++i) {
            written_bytes += iov[i].iov_len;
        }
        return under_->Writev(iov, iov_size, nwrite);
    }
    std::string LocalAddr() override { return under_->LocalAddr(); }
    std::string RemoteAddr() override { return under_->RemoteAddr(); }
    void SetRecvTimeout(int64_t timeout_us) override {
        recv_timeout = timeout_us;
        under_->SetRecvTimeout(timeout_us);
    }
    void SetSendTimeout(int64_t timeout_us) override { under_->SetSendTimeout(timeout_us); }

    size_t read_bytes = 0;
    size_t written_bytes = 0;
    int64_t recv_timeout = 0;

 private:
    std::unique_ptr<StreamConn> under_;
};

int Echo(StreamConn &c) {
    char buf[1024];
    ssize_t n = 0;
    int ret;
    while ((ret = c.Read(buf, sizeof(buf), &n)) == COCO_SUCCESS) {
        if ((ret = c.Write(buf, n, nullptr)) != COCO_SUCCESS) {
            break;
        }
    }
    return ret;
}

TcpServerOptions TlsOptions() {
    TcpServerOptions opt;
    opt.tls_key_file = COCO_SOURCE_DIR "/examples/http-server/server.key";
    opt.tls_crt_file = COCO_SOURCE_DIR "/examples/http-server/server.crt";
    opt.recv_timeout_us = kTimeoutUs;
    opt.send_timeout_us = kTimeoutUs;
    return opt;
}

std::shared_ptr<TlsConfig> ClientConfig() {
    std::shared_ptr<TlsConfig> cfg;
    CHECK_EQ(TlsConfig::NewClient(&cfg), COCO_SUCCESS);
    return cfg;
}

// Connects over TCP but leaves the handshake to the caller or to the first Read or Write.
std::unique_ptr<TlsConn> DialTlsLazy(int port) {
    std::unique_ptr<TcpConn> tcp;
    if (DialTcp(kLoopback, port, kTimeoutUs, &tcp) != COCO_SUCCESS) {
        return nullptr;
    }
    std::unique_ptr<TlsConn> tls(new TlsConn(std::move(tcp), ClientConfig()));
    tls->SetTimeout(kTimeoutUs);
    return tls;
}

std::unique_ptr<TlsConn> DialTls(int port) {
    std::unique_ptr<TlsConn> tls = DialTlsLazy(port);
    if (tls && tls->Handshake() != COCO_SUCCESS) {
        tls.reset();
    }
    return tls;
}

}  // namespace

// ReadFully fills the buffer with plaintext even when it arrives in several records.
COTEST(TlsReadFullyReturnsPlaintext) {
    const int port = 19211;
    TcpServer server(
        [](StreamConn &c) {
            c.Write((void *)"01234", 5, nullptr);
            CocoSleepMs(5);
            c.Write((void *)"56789", 5, nullptr);
            char b;
            ssize_t n = 0;
            return c.Read(&b, 1, &n);
        },
        TlsOptions());
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    std::unique_ptr<TlsConn> ssl = DialTls(port);
    CHECK(ssl != nullptr);
    if (!ssl) {
        return;
    }
    char buf[10];
    ssize_t n = 0;
    CHECK_EQ(ssl->ReadFully(buf, sizeof(buf), &n), COCO_SUCCESS);
    CHECK_EQ(n, 10);
    CHECK(std::string(buf, sizeof(buf)) == "0123456789");
}

// Two coroutines write large payloads over one TLS connection. Each flush yields on the
// full socket buffer while the other queues more records; every byte must still arrive
// and decrypt.
COTEST(TlsConcurrentWriters) {
    const int port = 19212;
    const size_t kChunk = 16 * 1024;
    const int kChunks = 256;
    const size_t kTotal = 2 * kChunk * kChunks;

    size_t got_a = 0, got_b = 0, got_other = 0;
    int read_err = COCO_SUCCESS;
    TcpServer server(
        [&](StreamConn &c) {
            char buf[8192];
            while (got_a + got_b + got_other < kTotal) {
                ssize_t n = 0;
                if ((read_err = c.Read(buf, sizeof(buf), &n)) != COCO_SUCCESS) {
                    break;
                }
                for (ssize_t i = 0; i < n; ++i) {
                    if (buf[i] == 'a') {
                        ++got_a;
                    } else if (buf[i] == 'b') {
                        ++got_b;
                    } else {
                        ++got_other;
                    }
                }
            }
            return read_err;
        },
        TlsOptions());
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    std::unique_ptr<TlsConn> ssl = DialTls(port);
    CHECK(ssl != nullptr);
    if (!ssl) {
        return;
    }

    int err_a = COCO_SUCCESS, err_b = COCO_SUCCESS;
    auto writer = [&](char fill, int *err) {
        return [&, fill, err]() {
            std::string chunk(kChunk, fill);
            for (int i = 0; i < kChunks && *err == COCO_SUCCESS; ++i) {
                *err = ssl->Write(&chunk[0], chunk.size(), nullptr);
            }
        };
    };
    st_thread_t a = cotest::Go(writer('a', &err_a));
    st_thread_t b = cotest::Go(writer('b', &err_b));
    st_thread_join(a, NULL);
    st_thread_join(b, NULL);

    CHECK_EQ(err_a, COCO_SUCCESS);
    CHECK_EQ(err_b, COCO_SUCCESS);
    CHECK(cotest::WaitUntil([&]() { return got_a + got_b + got_other >= kTotal; }, 3000));
    CHECK_EQ(read_err, COCO_SUCCESS);
    CHECK_EQ(got_a, kChunk * kChunks);
    CHECK_EQ(got_b, kChunk * kChunks);
    CHECK_EQ(got_other, 0);
}

// TLS runs over a StreamConn that is not a socket: every ciphertext byte, handshake
// included, goes through it, and timeouts set on the TlsConn reach it.
COTEST(TlsOverAnyStreamConn) {
    const int port = 19213;
    TcpServer server(Echo, TlsOptions());
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    std::unique_ptr<TcpConn> tcp;
    CHECK_EQ(DialTcp(kLoopback, port, kTimeoutUs, &tcp), COCO_SUCCESS);
    if (!tcp) {
        return;
    }
    CountingConn *counting = new CountingConn(std::move(tcp));
    TlsConn tls(std::unique_ptr<StreamConn>(counting), ClientConfig());
    tls.SetTimeout(kTimeoutUs);
    CHECK_EQ(counting->recv_timeout, kTimeoutUs);

    CHECK_EQ(tls.Handshake(), COCO_SUCCESS);
    size_t handshake_read = counting->read_bytes;
    size_t handshake_written = counting->written_bytes;
    CHECK(handshake_read > 0);
    CHECK(handshake_written > 0);

    CHECK_EQ(tls.Write((void *)"hello", 5, nullptr), COCO_SUCCESS);
    char buf[5];
    ssize_t n = 0;
    CHECK_EQ(tls.ReadFully(buf, sizeof(buf), &n), COCO_SUCCESS);
    CHECK(std::string(buf, n) == "hello");
    // Records carry a header and a tag, so more than the plaintext crossed the wire.
    CHECK(counting->written_bytes - handshake_written > 5);
    CHECK(counting->read_bytes - handshake_read > 5);
}

// TlsDialer stacks on any dialer: it returns a handshaken connection whose ciphertext went
// through the connection the under-dialer made.
COTEST(TlsDialerOverAnyDialer) {
    const int port = 19216;
    TcpServer server(Echo, TlsOptions());
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    CountingConn *counting = nullptr;
    StreamDialer tcp = TcpDialer();
    StreamDialer under = [&](const std::string &host, int p, int64_t timeout_us,
                             std::unique_ptr<StreamConn> *conn) {
        std::unique_ptr<StreamConn> raw;
        int ret = tcp(host, p, timeout_us, &raw);
        if (ret == COCO_SUCCESS) {
            counting = new CountingConn(std::move(raw));
            conn->reset(counting);
        }
        return ret;
    };

    std::unique_ptr<StreamConn> conn;
    CHECK_EQ(TlsDialer(nullptr, under)(kLoopback, port, kTimeoutUs, &conn), COCO_SUCCESS);
    if (!conn) {
        return;
    }
    CHECK(counting != nullptr && counting->read_bytes > 0);
    CHECK(counting != nullptr && counting->recv_timeout == kTimeoutUs);
    CHECK_EQ(conn->Write((void *)"dial", 4, nullptr), COCO_SUCCESS);
    char buf[4];
    ssize_t n = 0;
    CHECK_EQ(conn->ReadFully(buf, sizeof(buf), &n), COCO_SUCCESS);
    CHECK(std::string(buf, n) == "dial");
}

// Without Handshake(), the first Write handshakes before sending.
COTEST(TlsHandshakesOnFirstUse) {
    const int port = 19214;
    TcpServer server(Echo, TlsOptions());
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    std::unique_ptr<TlsConn> tls = DialTlsLazy(port);
    CHECK(tls != nullptr);
    if (!tls) {
        return;
    }
    CHECK_EQ(tls->Write((void *)"lazy", 4, nullptr), COCO_SUCCESS);
    char buf[4];
    ssize_t n = 0;
    CHECK_EQ(tls->ReadFully(buf, sizeof(buf), &n), COCO_SUCCESS);
    CHECK(std::string(buf, n) == "lazy");
    CHECK_EQ(tls->Handshake(), COCO_SUCCESS);
}

// A peer that connects and never sends a ClientHello holds up only its own connection:
// TlsListener::Accept does not handshake, so the next client is served.
COTEST(TlsSilentPeerDoesNotBlockAccept) {
    const int port = 19215;
    TcpServer server(Echo, TlsOptions());
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    std::unique_ptr<TcpConn> silent;
    CHECK_EQ(DialTcp(kLoopback, port, kTimeoutUs, &silent), COCO_SUCCESS);
    CHECK(cotest::WaitUntil([&]() { return server.ConnCount() == 1; }));

    std::unique_ptr<TlsConn> tls = DialTls(port);
    CHECK(tls != nullptr);
    if (!tls) {
        return;
    }
    CHECK_EQ(tls->Write((void *)"next", 4, nullptr), COCO_SUCCESS);
    char buf[4];
    ssize_t n = 0;
    CHECK_EQ(tls->ReadFully(buf, sizeof(buf), &n), COCO_SUCCESS);
    CHECK(std::string(buf, n) == "next");
    CHECK_EQ(server.ConnCount(), 2);
}
