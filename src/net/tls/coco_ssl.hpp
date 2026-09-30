#pragma once

#include <openssl/ssl.h>

#include <vector>

#include "base/coroutine_mgr.hpp"
#include "net/layer4/coco_layer4.hpp"

// The SSL connection over TCP transport. One coroutine may read while others write:
// ciphertext produced by either side goes out in order, one flush at a time.
class SslConn : public StreamConn {
 public:
    SslConn(st_netfd_t _stfd, StreamConn* under_layer);
    virtual ~SslConn();

    int Read(void* buf, size_t size, ssize_t* nread);
    int Write(void* buf, size_t size, ssize_t* nwrite);
    int Writev(const iovec* iov, int iov_size, ssize_t* nwrite);
    int ReadFully(void* buf, size_t size, ssize_t* nread);
    std::string RemoteAddr();

 protected:
    // Drive SSL_do_handshake until done, independent of TLS version and flight layout.
    int DoHandshake();
    // Send whatever SSL has queued in bio_out to the peer.
    int FlushOutput();
    // Serializes flushes: SSL_read and SSL_write both queue records, and a flush yields.
    st_mutex_t flush_lock_ = nullptr;
    // Ciphertext taken out of bio_out, so a flush never writes from memory SSL may move.
    std::vector<char> flush_buf_;
    // The first failed flush; the stream is broken for every later writer.
    int flush_err_ = 0;

    // The under-layer plaintext transport.
    StreamConn* under_layer_ = nullptr;
    SSL_CTX* ssl_ctx;
    SSL* ssl;
    BIO* bio_in = nullptr;
    BIO* bio_out = nullptr;
};

class SslServer : public SslConn {
 public:
    SslServer(st_netfd_t _stfd, StreamConn* under_layer);
    virtual ~SslServer() = default;

    int Handshake(std::string key_file, std::string crt_file);
};

class SslClient : public SslConn {
 public:
    SslClient(st_netfd_t _stfd, StreamConn* under_layer);
    virtual ~SslClient() = default;

    int Handshake();
};