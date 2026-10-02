#pragma once

#include <memory>
#include <string>
#include <vector>

#include "coco/base/st_fwd.hpp"

// OpenSSL's handle types, as <openssl/types.h> declares them; include <openssl/ssl.h> to
// use TlsConfig::ctx().
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct bio_st BIO;

#include "coco/net/layer4/coco_layer4.hpp"

namespace coco {

// The SSL_CTX every connection made with it shares, so certificates are loaded once.
class TlsConfig {
 public:
    ~TlsConfig();

    TlsConfig(const TlsConfig &) = delete;
    TlsConfig &operator=(const TlsConfig &) = delete;

    // For accepting: loads a PEM private key and certificate chain.
    static int NewServer(const std::string &key_file, const std::string &crt_file,
                         std::shared_ptr<TlsConfig> *cfg);
    // For connecting. The peer's certificate is not verified until
    // EnablePeerVerification().
    static int NewClient(std::shared_ptr<TlsConfig> *cfg);

    // Client only. Turns on SSL_VERIFY_PEER and loads the default CA paths.
    // TlsDialer then sends SNI and checks the certificate hostname.
    int EnablePeerVerification();
    bool VerifyPeer() const { return verify_peer_; }

    bool IsServer() const { return server_; }
    SSL_CTX *ctx() const { return ctx_; }

 private:
    TlsConfig(SSL_CTX *ctx, bool server) : ctx_(ctx), server_(server) {}

    SSL_CTX *ctx_;
    bool server_;
    bool verify_peer_ = false;
};

// TLS over any StreamConn, as the side the config is for. The handshake runs once, on the
// first Read or Write or on Handshake(); ciphertext only goes through the under-layer.
// One coroutine may read while others write: ciphertext produced by either side goes out
// in order, one flush at a time.
class TlsConn : public StreamConn {
 public:
    TlsConn(std::unique_ptr<StreamConn> under, std::shared_ptr<TlsConfig> cfg);
    virtual ~TlsConn();

    // Returns the handshake's result, running it if no one has yet.
    int Handshake();

    // Hostname for SNI. When the config verifies peers, the certificate must
    // match this name. Call before the handshake.
    void SetPeerName(const std::string &name) { peer_name_ = name; }

    int Read(void *buf, size_t size, ssize_t *nread) override;
    int Write(void *buf, size_t size, ssize_t *nwrite) override;
    int Writev(const iovec *iov, int iov_size, ssize_t *nwrite) override;
    std::string LocalAddr() override { return under_->LocalAddr(); }
    std::string RemoteAddr() override { return under_->RemoteAddr(); }
    void SetRecvTimeout(int64_t timeout_us) override { under_->SetRecvTimeout(timeout_us); }
    void SetSendTimeout(int64_t timeout_us) override { under_->SetSendTimeout(timeout_us); }

 private:
    int Setup();
    // Drive SSL_do_handshake until done, independent of TLS version and flight layout.
    int DoHandshake();
    // Send whatever SSL has queued in bio_out to the peer.
    int FlushOutput();

    std::unique_ptr<StreamConn> under_;
    std::shared_ptr<TlsConfig> cfg_;
    std::string peer_name_;
    SSL *ssl_ = nullptr;
    BIO *bio_in_ = nullptr;
    BIO *bio_out_ = nullptr;

    // Held while the handshake runs, so a reader and a writer do not both start it.
    st_mutex_t handshake_lock_ = nullptr;
    bool handshake_done_ = false;
    int handshake_err_ = 0;

    // Serializes flushes: SSL_read and SSL_write both queue records, and a flush yields.
    st_mutex_t flush_lock_ = nullptr;
    // Ciphertext taken out of bio_out, so a flush never writes from memory SSL may move.
    std::vector<char> flush_buf_;
    // The first failed flush; the stream is broken for every later writer.
    int flush_err_ = 0;

    // Ciphertext read from the under-layer, used by one reader at a time.
    std::vector<char> read_buf_;
};

// Dials with under, TcpDialer() when empty, then handshakes as a client with cfg, one
// config shared by all such dialers when null. The handshake is bounded by the dial's
// timeout.
StreamDialer TlsDialer(std::shared_ptr<TlsConfig> cfg = nullptr, StreamDialer under = nullptr);

// Wraps every connection the inner listener accepts in a TlsConn. Accept does not
// handshake, so a slow peer holds up only its own connection.
class TlsListener : public StreamListener {
 public:
    TlsListener(std::unique_ptr<StreamListener> inner, std::shared_ptr<TlsConfig> cfg);
    virtual ~TlsListener() = default;

    int Accept(std::unique_ptr<StreamConn> *conn) override;
    std::string Addr() override { return inner_->Addr(); }

 private:
    std::unique_ptr<StreamListener> inner_;
    std::shared_ptr<TlsConfig> cfg_;
};

}  // namespace coco
