#include "coco/net/tls/config.hpp"

#include <openssl/ssl.h>

#include "coco/common/error.hpp"
#include "coco/log/log.hpp"

namespace coco {

static SSL_CTX* NewSslCtx() {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
#else
    OPENSSL_init_ssl(0, NULL);
#endif

#if (OPENSSL_VERSION_NUMBER <= 0x100020cfL)  // v1.0.2
    SSL_CTX* ctx = SSL_CTX_new(TLSv1_method());
#else
    SSL_CTX* ctx = SSL_CTX_new(TLS_method());
#endif
    if (ctx == NULL) {
        coco_error("SSL_CTX_new");
        return NULL;
    }
    coco_info("ssl: %s", OPENSSL_VERSION_TEXT);

    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    if (SSL_CTX_set_cipher_list(ctx, "ALL") != 1) {
        coco_error("SSL_CTX_set_cipher_list");
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

TlsConfig::~TlsConfig() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

int TlsConfig::NewServer(const std::string& key_file, const std::string& crt_file,
                         std::shared_ptr<TlsConfig>* cfg) {
    SSL_CTX* ctx = NewSslCtx();
    if (ctx == NULL) {
        return ERROR_HTTPS_HANDSHAKE;
    }
    std::shared_ptr<TlsConfig> c(new TlsConfig(ctx, true));

    if (SSL_CTX_use_certificate_chain_file(ctx, crt_file.c_str()) != 1) {
        coco_error("use cert %s", crt_file.c_str());
        return ERROR_HTTPS_KEY_CRT;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        coco_error("use key %s", key_file.c_str());
        return ERROR_HTTPS_KEY_CRT;
    }

    if (SSL_CTX_check_private_key(ctx) != 1) {
        coco_error("check key %s with cert %s", key_file.c_str(), crt_file.c_str());
        return ERROR_HTTPS_KEY_CRT;
    }
    coco_info("ssl: use key %s and cert %s", key_file.c_str(), crt_file.c_str());

    *cfg = c;
    return COCO_SUCCESS;
}

int TlsConfig::NewClient(std::shared_ptr<TlsConfig>* cfg) {
    SSL_CTX* ctx = NewSslCtx();
    if (ctx == NULL) {
        return ERROR_HTTPS_HANDSHAKE;
    }
    cfg->reset(new TlsConfig(ctx, false));
    return COCO_SUCCESS;
}

int TlsConfig::EnablePeerVerification() {
    if (server_ || ctx_ == nullptr) {
        return ERROR_HTTPS_HANDSHAKE;
    }
    // Best effort: a later SSL_CTX_load_verify_locations can still add CAs.
    SSL_CTX_set_default_verify_paths(ctx_);
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, NULL);
    verify_peer_ = true;
    return COCO_SUCCESS;
}

}  // namespace coco
