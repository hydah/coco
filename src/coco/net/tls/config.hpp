#pragma once

#include <memory>
#include <string>

// OpenSSL's handle types, as <openssl/types.h> declares them; include <openssl/ssl.h> to
// use TlsConfig::ctx().
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct bio_st BIO;

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

}  // namespace coco
