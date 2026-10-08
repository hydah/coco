#pragma once

#include <memory>
#include <string>
#include <vector>

#include "coco/base/st_fwd.hpp"
#include "coco/net/conn.hpp"
#include "coco/net/tls/config.hpp"

namespace coco {

// TLS over any StreamConn, as the side the config is for. The handshake runs once, on the
// first Read or Write or on Handshake(); ciphertext only goes through the under-layer.
// One coroutine may read while others write: ciphertext produced by either side goes out
// in order, one flush at a time.
class TlsConn : public StreamConn {
 public:
    TlsConn(std::unique_ptr<StreamConn> under, std::shared_ptr<TlsConfig> cfg);
    // Borrows under, which must outlive the TlsConn.
    TlsConn(StreamConn *under, std::shared_ptr<TlsConfig> cfg);
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

    // Null when under_ is borrowed.
    std::unique_ptr<StreamConn> owned_;
    StreamConn *under_;
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

// Wraps every connection the inner listener accepts in a TlsConn, like Go's
// tls.NewListener. Accept does not handshake, so a slow peer holds up only its own
// connection. A TcpServer with threads cannot take it, as a TlsConn cannot change threads:
// use TlsHandler() there.
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

// Serves TLS in front of next, like the handshake at the start of Go's http (*conn).serve:
// handshakes on conn as the server side of cfg, then runs next on the plaintext
// connection. A failed handshake returns its error without calling next.
//
//   TcpServer server(TlsHandler(cfg, Echo));
//
// The handshake runs on the connection's own coroutine and thread, so a silent peer holds
// up only its own connection and TcpServerOptions::threads works; it is bounded by the
// timeouts the TcpServer set on conn (none by default).
StreamHandler TlsHandler(std::shared_ptr<TlsConfig> cfg, StreamHandler next);

}  // namespace coco
