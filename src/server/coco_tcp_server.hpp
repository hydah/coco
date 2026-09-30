#pragma once

#include <functional>
#include <memory>
#include <string>

#include "base/coroutine.hpp"
#include "base/coroutine_mgr.hpp"
#include "net/layer4/coco_layer4.hpp"

// Serves one accepted connection on its own coroutine. The connection is closed when the
// handler returns; the return value is only logged.
typedef std::function<int(StreamConn &conn)> StreamHandler;

struct TcpServerOptions {
    // Applied to every connection before the handler runs; they also bound the TLS
    // handshake.
    int64_t recv_timeout_us = (int64_t)ST_UTIME_NO_TIMEOUT;
    int64_t send_timeout_us = (int64_t)ST_UTIME_NO_TIMEOUT;
    // When both are set, Serve() loads them once and wraps the listener in a TlsListener.
    // The handshake runs on the connection's coroutine, and the handler only ever sees
    // plaintext.
    std::string tls_key_file;
    std::string tls_crt_file;
};

// Accepts connections and runs the handler for each of them, one coroutine per
// connection. The handler, and everything it captures, must outlive the server.
class TcpServer {
 public:
    explicit TcpServer(StreamHandler handler, TcpServerOptions options = TcpServerOptions());
    // Stops the server, then closes the listening socket.
    virtual ~TcpServer();

    // Listens on ip:port over TCP and starts accepting.
    int ListenAndServe(const std::string &ip, int port);
    // Takes ownership of l and starts accepting on it.
    int Serve(std::unique_ptr<StreamListener> l);
    // Stops accepting, interrupts every connection and waits until all have exited. A
    // stopped server cannot be started again. Must not be called from a handler.
    void Stop();
    size_t ConnCount() const { return manager_.Size(); }

 private:
    class Acceptor;
    class Session;

    StreamHandler handler_;
    TcpServerOptions options_;
    ConnManager manager_;
    std::unique_ptr<StreamListener> listener_;
    Acceptor *acceptor_ = nullptr;
};
