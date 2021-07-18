#include "net/layer4/coco_ssl.hpp"

#include <memory>

#include "log/log.hpp"
#include "common/error.hpp"

SslConn::SslConn(st_netfd_t _stfd, StreamConn* under_layer) : StreamConn(_stfd)
{
    //  接管under_layer_ skt
    under_layer_ = under_layer;
    under_layer_->Release();
    
    ssl_ctx = NULL;
    ssl = NULL;
}

SslConn::~SslConn()
{
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
}

int SslConn::ReadFully(void* buf, size_t size, ssize_t* nread)
{
    return skt_->ReadFully(buf, size, nread);
}

int SslConn::Read(void* plaintext, size_t nn_plaintext, ssize_t* nread)
{
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
            coco_error("SSL_read r0=%d, r1=%d, r2=%lu, r3=%d",
                r0, r1, r2, r3);
            return ERROR_HTTPS_READ;
        }
    }
}

int SslConn::Write(void* plaintext, size_t nn_plaintext, ssize_t* nwrite)
{
    int err = COCO_SUCCESS;

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
        if (nwrite) {
            *nwrite += (ssize_t)r0;
        }

        uint8_t* data = NULL;
        int size = BIO_get_mem_data(bio_out, &data);
        if ((err = skt_->Write(data, size, NULL)) != COCO_SUCCESS) {
            coco_error("https: write data=%p, size=%d", data, size);
            return err;
        }
        if ((r0 = BIO_reset(bio_out)) != 1) {
            coco_error("BIO_reset r0=%d", r0);
            return ERROR_HTTPS_WRITE;
        }
    }

    return err;
}

int SslConn::Writev(const iovec *iov, int iov_size, ssize_t* nwrite)
{
    int err = COCO_SUCCESS;

    for (int i = 0; i < iov_size; i++) {
        const iovec* p = iov + i;
        if ((err = Write((void*)p->iov_base, (size_t)p->iov_len, nwrite)) != COCO_SUCCESS) {
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

SslServer::SslServer(st_netfd_t _stfd, StreamConn* under_layer) : SslConn(_stfd, under_layer) {}

int SslServer::Handshake(std::string key_file, std::string crt_file)
{
    int err = COCO_SUCCESS;

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
#else
    OPENSSL_init_ssl(0, NULL);
#endif
    // For HTTPS, try to connect over security transport.
#if (OPENSSL_VERSION_NUMBER < 0x10002000L) // v1.0.2
    ssl_ctx = SSL_CTX_new(TLS_method());
    coco_info("ssl v1.0.2");
#else
    ssl_ctx = SSL_CTX_new(TLSv1_2_method());
    coco_info("ssl v1.1.1");
#endif
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
    assert(SSL_CTX_set_cipher_list(ssl_ctx, "ALL") == 1);

    // TODO: Setup callback, see SSL_set_ex_data and SSL_set_info_callback
    if ((ssl = SSL_new(ssl_ctx)) == NULL) {
        coco_error("SSL_new ssl");
        return ERROR_HTTPS_HANDSHAKE;
    }

    if ((bio_in = BIO_new(BIO_s_mem())) == NULL) {
        coco_error("BIO_new in");
        return ERROR_HTTPS_HANDSHAKE;
    }

    if ((bio_out = BIO_new(BIO_s_mem())) == NULL) {
        BIO_free(bio_in);
        coco_error("BIO_new out");
        return ERROR_HTTPS_HANDSHAKE;
    }

    SSL_set_bio(ssl, bio_in, bio_out);

    // SSL setup active, as server role.
    SSL_set_accept_state(ssl);
    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);

    uint8_t* data = NULL;
    int r0, r1, size;

    // Setup the key and cert file for server.
    if ((r0 = SSL_use_certificate_file(ssl, crt_file.c_str(), SSL_FILETYPE_PEM)) != 1) {
        coco_error("use cert %s", crt_file.c_str());
        return ERROR_HTTPS_KEY_CRT;
    }

    if ((r0 = SSL_use_RSAPrivateKey_file(ssl, key_file.c_str(), SSL_FILETYPE_PEM)) != 1) {
        coco_error("use key %s", key_file.c_str());
        return ERROR_HTTPS_KEY_CRT;

    }

    if ((r0 = SSL_check_private_key(ssl)) != 1) {
        coco_error("check key %s with cert %s",
            key_file.c_str(), crt_file.c_str());
                return ERROR_HTTPS_KEY_CRT;

    }
    coco_info("ssl: use key %s and cert %s", key_file.c_str(), crt_file.c_str());

    // Receive ClientHello
    while (true) {
        char buf[1024]; ssize_t nn = 0;
        if ((err = skt_->Read(buf, sizeof(buf), &nn)) != COCO_SUCCESS) {
            coco_error("handshake: read");
            return err;
        }

        if ((r0 = BIO_write(bio_in, buf, (int)nn)) <= 0) {
            // TODO: 0 or -1 maybe block, use BIO_should_retry to check.
            coco_error("BIO_write r0=%d, data=%p, size=%d", r0, buf, (int)nn);
                return ERROR_HTTPS_HANDSHAKE;

        }

        r0 = SSL_do_handshake(ssl); r1 = SSL_get_error(ssl, r0);
        if (r0 != -1 || r1 != SSL_ERROR_WANT_READ) {
            coco_error("handshake r0=%d, r1=%d", r0, r1);
                return ERROR_HTTPS_HANDSHAKE;

        }

        if ((size = BIO_get_mem_data(bio_out, &data)) > 0) {
            // OK, reset it for the next write.
            if ((r0 = BIO_reset(bio_in)) != 1) {
                coco_error("BIO_reset r0=%d", r0);
                return ERROR_HTTPS_HANDSHAKE;
            }
            break;
        }
    }

    coco_info("https: ClientHello done");

    // Send ServerHello, Certificate, Server Key Exchange, Server Hello Done
    size = BIO_get_mem_data(bio_out, &data);
    if (!data || size <= 0) {
        coco_error("handshake data=%p, size=%d", data, size);
        return ERROR_HTTPS_HANDSHAKE;
    }
    if ((err = skt_->Write(data, size, NULL)) != COCO_SUCCESS) {
        coco_error("handshake: write data=%p, size=%d", data, size);
        return err;
    }
    if ((r0 = BIO_reset(bio_out)) != 1) {
        coco_error("BIO_reset r0=%d", r0);
        return ERROR_HTTPS_HANDSHAKE;
    }

    coco_info("https: ServerHello done");

    // Receive Client Key Exchange, Change Cipher Spec, Encrypted Handshake Message
    while (true) {
        char buf[1024]; ssize_t nn = 0;
        if ((err = skt_->Read(buf, sizeof(buf), &nn)) != COCO_SUCCESS) {
            coco_error("handshake: read");
            return err;
        }

        if ((r0 = BIO_write(bio_in, buf, (int)nn)) <= 0) {
            // TODO: 0 or -1 maybe block, use BIO_should_retry to check.
            coco_error("BIO_write r0=%d, data=%p, size=%d", r0, buf, (int)nn);
            return ERROR_HTTPS_HANDSHAKE;
        }

        r0 = SSL_do_handshake(ssl); r1 = SSL_get_error(ssl, r0);
        if (r0 == 1 && r1 == SSL_ERROR_NONE) {
            break;
        }

        if (r0 != -1 || r1 != SSL_ERROR_WANT_READ) {
            coco_error("handshake r0=%d, r1=%d", r0, r1);
            return ERROR_HTTPS_HANDSHAKE;
        }

        if ((size = BIO_get_mem_data(bio_out, &data)) > 0) {
            // OK, reset it for the next write.
            if ((r0 = BIO_reset(bio_in)) != 1) {
                coco_error("BIO_reset r0=%d", r0);
                return ERROR_HTTPS_HANDSHAKE;
            }
            break;
        }
    }

    coco_info("https: Client done");

    // Send New Session Ticket, Change Cipher Spec, Encrypted Handshake Message
    size = BIO_get_mem_data(bio_out, &data);
    if (!data || size <= 0) {
        coco_error("handshake data=%p, size=%d", data, size);
        return ERROR_HTTPS_HANDSHAKE;
    }
    if ((err = skt_->Write(data, size, NULL)) != COCO_SUCCESS) {
        coco_error("handshake: write data=%p, size=%d", data, size);
        return err;
    }
    if ((r0 = BIO_reset(bio_out)) != 1) {
        coco_error("BIO_reset r0=%d", r0);
        return ERROR_HTTPS_HANDSHAKE;
    }

    coco_info("https: Server done");

    return err;
}




SslClient::SslClient(st_netfd_t _stfd, StreamConn* under_layer) : SslConn(_stfd, under_layer) {}

int SslClient::Handshake()
{
    int err = COCO_SUCCESS;

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
#else
    OPENSSL_init_ssl(0, NULL);
#endif
    // For HTTPS, try to connect over security transport.
#if (OPENSSL_VERSION_NUMBER < 0x10002000L) // v1.0.2
    ssl_ctx = SSL_CTX_new(TLS_method());
    coco_info("ssl v1.0.2");
#else
    ssl_ctx = SSL_CTX_new(TLSv1_2_method());
    coco_info("ssl v1.1.1");
#endif
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
    assert(SSL_CTX_set_cipher_list(ssl_ctx, "ALL") == 1);

    // TODO: Setup callback, see SSL_set_ex_data and SSL_set_info_callback
    if ((ssl = SSL_new(ssl_ctx)) == NULL) {
        coco_error("SSL_new ssl");
        return ERROR_HTTPS_HANDSHAKE;
    }

    if ((bio_in = BIO_new(BIO_s_mem())) == NULL) {
        coco_error("BIO_new in");
        return ERROR_HTTPS_HANDSHAKE;
    }

    if ((bio_out = BIO_new(BIO_s_mem())) == NULL) {
        BIO_free(bio_in);
        coco_error("BIO_new out");
        return ERROR_HTTPS_HANDSHAKE;
    }

    SSL_set_bio(ssl, bio_in, bio_out);

     // SSL setup active, as client role.
    SSL_set_connect_state(ssl);
    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);

    // Send ClientHello.
    int r0 = SSL_do_handshake(ssl);
    int r1 = SSL_get_error(ssl, r0);
    if (r0 != -1 || r1 != SSL_ERROR_WANT_READ) {
        coco_error("handshake r0=%d, r1=%d", r0, r1);
        return ERROR_HTTPS_HANDSHAKE;
    }

    uint8_t* data = NULL;
    int size = BIO_get_mem_data(bio_out, &data);
    if (!data || size <= 0) {
        coco_error("handshake data=%p, size=%d", data, size);
        return ERROR_HTTPS_HANDSHAKE;
    }
    if ((err = skt_->Write(data, size, NULL)) != COCO_SUCCESS) {
        coco_error("handshake: write data=%p, size=%d", data, size);
        return err;
    }
    if ((r0 = BIO_reset(bio_out)) != 1) {
        coco_error("BIO_reset r0=%d", r0);
        return ERROR_HTTPS_HANDSHAKE;
    }

    coco_info("https: ClientHello done");

    // Receive ServerHello, Certificate, Server Key Exchange, Server Hello Done
    while (true) {
        char buf[512]; ssize_t nn = 0;
        if ((err = skt_->Read(buf, sizeof(buf), &nn)) != COCO_SUCCESS) {
            coco_error("handshake: read");
            return err;
        }

        if ((r0 = BIO_write(bio_in, buf, (int)nn)) <= 0) {
            // TODO: 0 or -1 maybe block, use BIO_should_retry to check.
            coco_error("BIO_write r0=%d, data=%p, size=%d", r0, buf, (int)nn);
            return ERROR_HTTPS_HANDSHAKE;
        }

        if ((r0 = SSL_do_handshake(ssl)) != -1 || (r1 = SSL_get_error(ssl, r0)) != SSL_ERROR_WANT_READ) {
            coco_error("handshake r0=%d, r1=%d", r0, r1);
            return ERROR_HTTPS_HANDSHAKE;
        }

        if ((size = BIO_get_mem_data(bio_out, &data)) > 0) {
            // OK, reset it for the next write.
            if ((r0 = BIO_reset(bio_in)) != 1) {
                coco_error("BIO_reset r0=%d", r0);
                return ERROR_HTTPS_HANDSHAKE;
            }
            break;
        }
    }

    coco_info("https: ServerHello done");

    // Send Client Key Exchange, Change Cipher Spec, Encrypted Handshake Message
    if ((err = skt_->Write(data, size, NULL)) != COCO_SUCCESS) {
        coco_error("handshake: write data=%p, size=%d", data, size);
        return err;
    }
    if ((r0 = BIO_reset(bio_out)) != 1) {
        coco_error("BIO_reset r0=%d", r0);
        return ERROR_HTTPS_HANDSHAKE;
    }

    coco_info("https: Client done");

    // Receive New Session Ticket, Change Cipher Spec, Encrypted Handshake Message
    while (true) {
        char buf[128];
        ssize_t nn = 0;
        if ((err = skt_->Read(buf, sizeof(buf), &nn)) != COCO_SUCCESS) {
            coco_error("handshake: read");
            return err;
        }

        if ((r0 = BIO_write(bio_in, buf, (int)nn)) <= 0) {
            // TODO: 0 or -1 maybe block, use BIO_should_retry to check.
            coco_error("BIO_write r0=%d, data=%p, size=%d", r0, buf, (int)nn);
            return ERROR_HTTPS_HANDSHAKE;
        }

        r0 = SSL_do_handshake(ssl); r1 = SSL_get_error(ssl, r0);
        if (r0 == 1 && r1 == SSL_ERROR_NONE) {
            break;
        }

        if (r0 != -1 || r1 != SSL_ERROR_WANT_READ) {
            coco_error("handshake r0=%d, r1=%d", r0, r1);
            return ERROR_HTTPS_HANDSHAKE;
        }
    }

    coco_info("https: Server done");

    return err;
}