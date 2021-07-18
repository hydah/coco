#pragma once

#include <openssl/ssl.h>

#include "net/layer4/coco_layer4.hpp"
#include "base/coroutine_mgr.hpp"

// The SSL connection over TCP transport, in server mode.
class SslServer : public StreamConn
{
public:
    SslServer(st_netfd_t _stfd, StreamConn* under_layer);
    virtual ~SslServer();

    virtual int Handshake(std::string key_file, std::string crt_file);
    virtual int Read(void *buf, size_t size, ssize_t *nread);
    virtual int Write(void *buf, size_t size, ssize_t *nwrite);
    virtual int Writev(const iovec *iov, int iov_size, ssize_t *nwrite);
    virtual int ReadFully(void* buf, size_t size, ssize_t* nread);
    virtual std::string RemoteAddr();

private:
    // The under-layer plaintext transport.
    StreamConn* under_layer_ = nullptr;
    SSL_CTX* ssl_ctx;
    SSL* ssl;
    BIO* bio_in;
    BIO* bio_out;
};