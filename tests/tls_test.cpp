// SslConn: plaintext ReadFully, and writers sharing one connection.

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
const int kTimeoutUs = 2000 * 1000;

TcpServerOptions TlsOptions() {
    TcpServerOptions opt;
    opt.tls_key_file = COCO_SOURCE_DIR "/examples/http-server/server.key";
    opt.tls_crt_file = COCO_SOURCE_DIR "/examples/http-server/server.crt";
    opt.recv_timeout_us = kTimeoutUs;
    opt.send_timeout_us = kTimeoutUs;
    return opt;
}

SslClient *DialTls(int port) {
    TcpConn *tcp = DialTcp(kLoopback, port, kTimeoutUs);
    if (!tcp) {
        return nullptr;
    }
    SslClient *ssl = new SslClient(tcp->GetStfd(), tcp);
    ssl->SetTimeout(kTimeoutUs);
    if (ssl->Handshake() != COCO_SUCCESS) {
        delete ssl;
        return nullptr;
    }
    return ssl;
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

    std::unique_ptr<SslClient> ssl(DialTls(port));
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

    std::unique_ptr<SslClient> ssl(DialTls(port));
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
