#include "net/tls/coco_tls.hpp"

#include <assert.h>

#include "common/error.hpp"
#include "log/log.hpp"
#include "net/layer4/coco_tcp.hpp"

// Ciphertext buffers; a TLS record is at most 16KB of payload.
#define TLS_IO_BUFFER_BYTES (16 * 1024)

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

TlsConn::TlsConn(std::unique_ptr<StreamConn> under, std::shared_ptr<TlsConfig> cfg)
    : under_(std::move(under)), cfg_(std::move(cfg)) {
    handshake_lock_ = st_mutex_new();
    flush_lock_ = st_mutex_new();
}

TlsConn::~TlsConn() {
    coco_dbg("destruct tls conn");
    if (ssl_) {
        // this function will free bio_in and bio_out
        SSL_free(ssl_);
        ssl_ = nullptr;
    }

    if (flush_lock_) {
        st_mutex_destroy(flush_lock_);
        flush_lock_ = nullptr;
    }

    if (handshake_lock_) {
        st_mutex_destroy(handshake_lock_);
        handshake_lock_ = nullptr;
    }
}

int TlsConn::Handshake() {
    if (handshake_done_) {
        return handshake_err_;
    }

    if (st_mutex_lock(handshake_lock_) != 0) {
        return ERROR_THREAD_INTERRUPED;
    }
    if (!handshake_done_) {
        int err = Setup();
        if (err == COCO_SUCCESS) {
            err = DoHandshake();
        }
        handshake_err_ = err;
        handshake_done_ = true;
    }
    st_mutex_unlock(handshake_lock_);

    return handshake_err_;
}

// Create ssl and its memory BIOs; on failure the destructor frees what was set.
int TlsConn::Setup() {
    // TODO: Setup callback, see SSL_set_ex_data and SSL_set_info_callback
    if ((ssl_ = SSL_new(cfg_->ctx())) == NULL) {
        coco_error("SSL_new ssl");
        return ERROR_HTTPS_HANDSHAKE;
    }

    if ((bio_in_ = BIO_new(BIO_s_mem())) == NULL) {
        coco_error("BIO_new in");
        return ERROR_HTTPS_HANDSHAKE;
    }

    if ((bio_out_ = BIO_new(BIO_s_mem())) == NULL) {
        BIO_free(bio_in_);
        bio_in_ = nullptr;
        coco_error("BIO_new out");
        return ERROR_HTTPS_HANDSHAKE;
    }

    SSL_set_bio(ssl_, bio_in_, bio_out_);
    SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE);

    if (cfg_->IsServer()) {
        SSL_set_accept_state(ssl_);
    } else {
        SSL_set_connect_state(ssl_);
    }
    return COCO_SUCCESS;
}

int TlsConn::Read(void* plaintext, size_t nn_plaintext, ssize_t* nread) {
    int err = COCO_SUCCESS;

    if ((err = Handshake()) != COCO_SUCCESS) {
        return err;
    }

    while (true) {
        int r0 = SSL_read(ssl_, plaintext, (int)nn_plaintext);
        int r1 = SSL_get_error(ssl_, r0);
        size_t r2 = BIO_ctrl_pending(bio_in_);
        int r3 = SSL_is_init_finished(ssl_);

        // OK, got data.
        if (r0 > 0) {
            assert(r0 <= (int)nn_plaintext);
            if (nread) {
                *nread = r0;
            }
            return err;
        }

        // Need to read more data to feed SSL.
        if (r0 == -1 && r1 == SSL_ERROR_WANT_READ) {
            // SSL_read may queue records to send, e.g. a TLS 1.3 KeyUpdate response.
            if ((err = FlushOutput()) != COCO_SUCCESS) {
                return err;
            }

            if (read_buf_.empty()) {
                read_buf_.resize(TLS_IO_BUFFER_BYTES);
            }
            ssize_t nn = 0;
            if ((err = under_->Read(read_buf_.data(), read_buf_.size(), &nn)) != COCO_SUCCESS) {
                coco_error("https: read");
                return err;
            }

            int r = BIO_write(bio_in_, read_buf_.data(), (int)nn);
            if (r <= 0) {
                // TODO: 0 or -1 maybe block, use BIO_should_retry to check.
                coco_error("BIO_write r0=%d, size=%d", r, (int)nn);
                return ERROR_HTTPS_READ;
            }
            continue;
        }

        // Fail for error.
        if (r0 <= 0) {
            coco_error("SSL_read r0=%d, r1=%d, r2=%lu, r3=%d", r0, r1, r2, r3);
            return ERROR_HTTPS_READ;
        }
    }
}

int TlsConn::Write(void* plaintext, size_t nn_plaintext, ssize_t* nwrite) {
    int err = COCO_SUCCESS;
    ssize_t written = 0;

    if ((err = Handshake()) != COCO_SUCCESS) {
        return err;
    }

    for (char* p = (char*)plaintext; p < (char*)plaintext + nn_plaintext;) {
        int left = (int)nn_plaintext - (int)(p - (char*)plaintext);
        int r0 = SSL_write(ssl_, (const void*)p, left);
        int r1 = SSL_get_error(ssl_, r0);
        if (r0 <= 0) {
            coco_error("https: write data=%p, size=%d, r0=%d, r1=%d", p, left, r0, r1);
            return ERROR_HTTPS_WRITE;
        }

        // Move p to the next writing position.
        p += r0;
        written += r0;
        if (nwrite) {
            *nwrite = written;
        }

        if ((err = FlushOutput()) != COCO_SUCCESS) {
            return err;
        }
    }

    return err;
}

int TlsConn::Writev(const iovec* iov, int iov_size, ssize_t* nwrite) {
    int err = COCO_SUCCESS;
    ssize_t total = 0;

    for (int i = 0; i < iov_size; i++) {
        const iovec* p = iov + i;
        ssize_t n = 0;
        err = Write((void*)p->iov_base, (size_t)p->iov_len, &n);
        total += n;
        if (nwrite) {
            *nwrite = total;
        }
        if (err != COCO_SUCCESS) {
            coco_error("write iov #%d base=%p, size=%d", i, p->iov_base, (int)p->iov_len);
            return err;
        }
    }

    return err;
}

int TlsConn::FlushOutput() {
    // Records queued by this caller are either still in bio_out, or were taken by the
    // flush holding the lock, which writes them before it unlocks.
    if (st_mutex_lock(flush_lock_) != 0) {
        return ERROR_THREAD_INTERRUPED;
    }

    int err = flush_err_;
    if (flush_buf_.empty()) {
        flush_buf_.resize(TLS_IO_BUFFER_BYTES);
    }
    while (err == COCO_SUCCESS && BIO_ctrl_pending(bio_out_) > 0) {
        int n = BIO_read(bio_out_, flush_buf_.data(), (int)flush_buf_.size());
        if (n <= 0) {
            coco_error("BIO_read r0=%d", n);
            err = ERROR_HTTPS_WRITE;
            break;
        }
        if ((err = under_->Write(flush_buf_.data(), n, NULL)) != COCO_SUCCESS) {
            coco_error("https: write size=%d", n);
        }
    }
    flush_err_ = err;

    st_mutex_unlock(flush_lock_);
    return err;
}

int TlsConn::DoHandshake() {
    int err = COCO_SUCCESS;

    while (true) {
        int r0 = SSL_do_handshake(ssl_);
        int r1 = SSL_get_error(ssl_, r0);

        // Send whatever this step produced, including the final flight when r0 == 1.
        if ((err = FlushOutput()) != COCO_SUCCESS) {
            return err;
        }

        if (r0 == 1) {
            coco_info("https: handshake done, %s", SSL_get_version(ssl_));
            return err;
        }

        if (r1 != SSL_ERROR_WANT_READ) {
            coco_error("handshake r0=%d, r1=%d", r0, r1);
            return ERROR_HTTPS_HANDSHAKE;
        }

        // Unconsumed bytes stay in bio_in, so never reset it: records that
        // arrive with the last flight (e.g. application data) must not be lost.
        char buf[4096];
        ssize_t nn = 0;
        if ((err = under_->Read(buf, sizeof(buf), &nn)) != COCO_SUCCESS) {
            coco_error("handshake: read");
            return err;
        }

        if ((r0 = BIO_write(bio_in_, buf, (int)nn)) <= 0) {
            coco_error("BIO_write r0=%d, data=%p, size=%d", r0, buf, (int)nn);
            return ERROR_HTTPS_HANDSHAKE;
        }
    }
}

StreamDialer TlsDialer(std::shared_ptr<TlsConfig> cfg, StreamDialer under) {
    if (!under) {
        under = TcpDialer();
    }
    return [cfg, under](const std::string& host, int port, int64_t timeout_us,
                        std::unique_ptr<StreamConn>* conn) {
        int ret = COCO_SUCCESS;

        std::shared_ptr<TlsConfig> c = cfg;
        if (!c) {
            static std::shared_ptr<TlsConfig> shared;
            if (!shared && (ret = TlsConfig::NewClient(&shared)) != COCO_SUCCESS) {
                return ret;
            }
            c = shared;
        }

        std::unique_ptr<StreamConn> raw;
        if ((ret = under(host, port, timeout_us, &raw)) != COCO_SUCCESS) {
            return ret;
        }
        raw->SetTimeout(timeout_us);

        std::unique_ptr<TlsConn> tls(new TlsConn(std::move(raw), c));
        if ((ret = tls->Handshake()) != COCO_SUCCESS) {
            coco_error("tls handshake with %s:%d failed. ret=%d", host.c_str(), port, ret);
            return ret;
        }
        conn->reset(tls.release());
        return ret;
    };
}

TlsListener::TlsListener(std::unique_ptr<StreamListener> inner, std::shared_ptr<TlsConfig> cfg)
    : inner_(std::move(inner)), cfg_(std::move(cfg)) {}

int TlsListener::Accept(std::unique_ptr<StreamConn>* conn) {
    std::unique_ptr<StreamConn> under;
    int ret = inner_->Accept(&under);
    if (ret == COCO_SUCCESS) {
        conn->reset(new TlsConn(std::move(under), cfg_));
    }
    return ret;
}
