#pragma once

#include <openssl/ssl.h>

#include "base/coroutine_mgr.hpp"
#include "net/layer4/coco_layer4.hpp"

// The SSL connection over TCP transport, in server mode.
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
    // The under-layer plaintext transport.
    StreamConn* under_layer_ = nullptr;
    SSL_CTX* ssl_ctx;
    SSL* ssl;
    BIO* bio_in;
    BIO* bio_out;
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