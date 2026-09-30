#include "net/tls/coco_ssl.hpp"

#include <memory>

#include "common/error.hpp"
#include "log/log.hpp"

SslConn::SslConn(st_netfd_t _stfd, StreamConn* under_layer) : StreamConn(_stfd) {
    //  接管under_layer_ skt
    under_layer_ = under_layer;
    under_layer_->Release();

    ssl_ctx = NULL;
    ssl = NULL;
    flush_lock_ = st_mutex_new();
}

SslConn::~SslConn() {
    coco_dbg("destruct ssl conn");
    if (ssl) {
        // this function will free bio_in and bio_out
        SSL_free(ssl);
        ssl = NULL;
    }

    if (ssl_ctx) {
        SSL_CTX_free(ssl_ctx);
        ssl_ctx = NULL;
    }

    //  接管under_layer_ skt
    if (under_layer_) {
        delete under_layer_;
        under_layer_ = nullptr;
    }

    if (flush_lock_) {
        st_mutex_destroy(flush_lock_);
        flush_lock_ = nullptr;
    }
}

int SslConn::ReadFully(void* buf, size_t size, ssize_t* nread) {
    int err = COCO_SUCCESS;
    size_t got = 0;
    while (got < size) {
        ssize_t n = 0;
        if ((err = Read((char*)buf + got, size - got, &n)) != COCO_SUCCESS) {
            break;
        }
        got += (size_t)n;
    }
    if (nread) {
        *nread = (ssize_t)got;
    }
    return err;
}

int SslConn::Read(void* plaintext, size_t nn_plaintext, ssize_t* nread) {
    int err = COCO_SUCCESS;

    while (true) {
        int r0 = SSL_read(ssl, plaintext, (int)nn_plaintext);
        int r1 = SSL_get_error(ssl, r0);
        size_t r2 = BIO_ctrl_pending(bio_in);
        int r3 = SSL_is_init_finished(ssl);

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

            // TODO: Can we avoid copy?
            int nn_cipher = (int)nn_plaintext;
            std::unique_ptr<char[]> cipher(new char[nn_cipher]);

            // Read the cipher from SSL.
            ssize_t nn = 0;
            if ((err = skt_->Read(cipher.get(), nn_cipher, &nn)) != COCO_SUCCESS) {
                coco_error("https: read");
                return err;
            }

            int r = BIO_write(bio_in, cipher.get(), (int)nn);
            if (r <= 0) {
                // TODO: 0 or -1 maybe block, use BIO_should_retry to check.
                coco_error("BIO_write r0=%d, cipher=%p, size=%d", r, cipher.get(), (int)nn);
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

int SslConn::Write(void* plaintext, size_t nn_plaintext, ssize_t* nwrite) {
    int err = COCO_SUCCESS;
    ssize_t written = 0;

    for (char* p = (char*)plaintext; p < (char*)plaintext + nn_plaintext;) {
        int left = (int)nn_plaintext - (int)(p - (char*)plaintext);
        int r0 = SSL_write(ssl, (const void*)p, left);
        int r1 = SSL_get_error(ssl, r0);
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

int SslConn::Writev(const iovec* iov, int iov_size, ssize_t* nwrite) {
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

std::string SslConn::RemoteAddr() {
    auto fd = skt_->get_osfd();
    return GetRemoteAddr(fd);
}

int SslConn::FlushOutput() {
    // Records queued by this caller are either still in bio_out, or were taken by the
    // flush holding the lock, which writes them before it unlocks.
    if (st_mutex_lock(flush_lock_) != 0) {
        return ERROR_THREAD_INTERRUPED;
    }

    int err = flush_err_;
    if (flush_buf_.empty()) {
        flush_buf_.resize(16 * 1024);
    }
    while (err == COCO_SUCCESS && BIO_ctrl_pending(bio_out) > 0) {
        int n = BIO_read(bio_out, flush_buf_.data(), (int)flush_buf_.size());
        if (n <= 0) {
            coco_error("BIO_read r0=%d", n);
            err = ERROR_HTTPS_WRITE;
            break;
        }
        if ((err = skt_->Write(flush_buf_.data(), n, NULL)) != COCO_SUCCESS) {
            coco_error("https: write size=%d", n);
        }
    }
    flush_err_ = err;

    st_mutex_unlock(flush_lock_);
    return err;
}

int SslConn::DoHandshake() {
    int err = COCO_SUCCESS;

    while (true) {
        int r0 = SSL_do_handshake(ssl);
        int r1 = SSL_get_error(ssl, r0);

        // Send whatever this step produced, including the final flight when r0 == 1.
        if ((err = FlushOutput()) != COCO_SUCCESS) {
            return err;
        }

        if (r0 == 1) {
            coco_info("https: handshake done, %s", SSL_get_version(ssl));
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
        if ((err = skt_->Read(buf, sizeof(buf), &nn)) != COCO_SUCCESS) {
            coco_error("handshake: read");
            return err;
        }

        if ((r0 = BIO_write(bio_in, buf, (int)nn)) <= 0) {
            coco_error("BIO_write r0=%d, data=%p, size=%d", r0, buf, (int)nn);
            return ERROR_HTTPS_HANDSHAKE;
        }
    }
}

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

// Create ssl and its memory BIOs; on failure the caller's destructor frees what was set.
static int SetupSsl(SSL_CTX* ctx, SSL** ssl, BIO** bio_in, BIO** bio_out) {
    // TODO: Setup callback, see SSL_set_ex_data and SSL_set_info_callback
    if ((*ssl = SSL_new(ctx)) == NULL) {
        coco_error("SSL_new ssl");
        return ERROR_HTTPS_HANDSHAKE;
    }

    if ((*bio_in = BIO_new(BIO_s_mem())) == NULL) {
        coco_error("BIO_new in");
        return ERROR_HTTPS_HANDSHAKE;
    }

    if ((*bio_out = BIO_new(BIO_s_mem())) == NULL) {
        BIO_free(*bio_in);
        *bio_in = NULL;
        coco_error("BIO_new out");
        return ERROR_HTTPS_HANDSHAKE;
    }

    SSL_set_bio(*ssl, *bio_in, *bio_out);
    SSL_set_mode(*ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);
    return COCO_SUCCESS;
}

SslServer::SslServer(st_netfd_t _stfd, StreamConn* under_layer) : SslConn(_stfd, under_layer) {}

int SslServer::Handshake(std::string key_file, std::string crt_file) {
    int err = COCO_SUCCESS;

    if ((ssl_ctx = NewSslCtx()) == NULL) {
        return ERROR_HTTPS_HANDSHAKE;
    }
    if ((err = SetupSsl(ssl_ctx, &ssl, &bio_in, &bio_out)) != COCO_SUCCESS) {
        return err;
    }

    // SSL setup active, as server role.
    SSL_set_accept_state(ssl);

    // Setup the key and cert file for server.
    if (SSL_use_certificate_file(ssl, crt_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        coco_error("use cert %s", crt_file.c_str());
        return ERROR_HTTPS_KEY_CRT;
    }

    if (SSL_use_PrivateKey_file(ssl, key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        coco_error("use key %s", key_file.c_str());
        return ERROR_HTTPS_KEY_CRT;
    }

    if (SSL_check_private_key(ssl) != 1) {
        coco_error("check key %s with cert %s", key_file.c_str(), crt_file.c_str());
        return ERROR_HTTPS_KEY_CRT;
    }
    coco_info("ssl: use key %s and cert %s", key_file.c_str(), crt_file.c_str());

    return DoHandshake();
}

SslClient::SslClient(st_netfd_t _stfd, StreamConn* under_layer) : SslConn(_stfd, under_layer) {}

int SslClient::Handshake() {
    int err = COCO_SUCCESS;

    if ((ssl_ctx = NewSslCtx()) == NULL) {
        return ERROR_HTTPS_HANDSHAKE;
    }
    if ((err = SetupSsl(ssl_ctx, &ssl, &bio_in, &bio_out)) != COCO_SUCCESS) {
        return err;
    }

    // SSL setup active, as client role.
    SSL_set_connect_state(ssl);

    return DoHandshake();
}
