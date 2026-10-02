#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "coco/base/coco_thread.hpp"
#include "coco/base/coroutine.hpp"
#include "coco/base/coroutine_mgr.hpp"
#include "coco/net/layer4/coco_layer4.hpp"

namespace coco {

class TcpConn;
class TcpListener;
class TlsConfig;

// Serves one accepted connection on its own coroutine. The connection is closed when the
// handler returns; the return value is only logged.
typedef std::function<int(StreamConn &conn)> StreamHandler;

struct TcpServerOptions {
    // Applied to every connection before the handler runs; they also bound the TLS
    // handshake.
    int64_t recv_timeout_us = kNoTimeout;
    int64_t send_timeout_us = kNoTimeout;
    // When both are set, Serve() loads them once and wraps the listener in a TlsListener.
    // The handshake runs on the connection's coroutine, and the handler only ever sees
    // plaintext.
    std::string tls_key_file;
    std::string tls_crt_file;
    // 0 or 1: the connections run on the server's thread. Above 1: the server's thread
    // only accepts, and each connection runs on whichever of this many worker threads
    // (CocoThread) has the fewest. The handler is then called on several threads at once,
    // so what it shares must be safe to use from all of them. Needs a TcpListener.
    int threads = 0;
};

// Accepts connections and runs the handler for each of them, one coroutine per
// connection. The handler, and everything it captures, must outlive the server.
//
//   TcpServer server(Echo);
//   return server.ListenAndServe("0.0.0.0", 8080);  // until Ctrl-C
//
// With TcpServerOptions::threads above 1 the connections are spread over worker threads.
class TcpServer {
 public:
    explicit TcpServer(StreamHandler handler, TcpServerOptions options = TcpServerOptions());
    // Stops the server, then closes the listening socket.
    virtual ~TcpServer();

    // Listen on ip:port over TCP, or take ownership of l, and serve until Stop() or a
    // shutdown (CocoShutdown(), SIGINT, SIGTERM), which stops the server before they
    // return COCO_SUCCESS.
    int ListenAndServe(const std::string &ip, int port);
    int Serve(std::unique_ptr<StreamListener> l);

    // The same, but return once accepting has started.
    int Start(const std::string &ip, int port);
    int Start(std::unique_ptr<StreamListener> l);
    // Blocks until Stop() or a shutdown; a shutdown stops the server first.
    void Wait();

    // Stops accepting and closes the listening socket, interrupts every connection and
    // waits until all have exited. When it returns, from whichever coroutine calls it, the
    // server is down; a second call waits for the first. A stopped server cannot be
    // started again. Must not be called from a handler, which may call CocoShutdown()
    // instead.
    void Stop();
    size_t ConnCount() const;

 private:
    class Acceptor;
    class Session;

    // Accepts one connection and starts serving it.
    int AcceptOne();
    // Moves conn to the least loaded worker.
    void Dispatch(std::unique_ptr<TcpConn> conn);
    // Runs on a worker: serves the connection on fd.
    void ServeFd(int fd);

    StreamHandler handler_;
    TcpServerOptions options_;
    ConnManager manager_;
    std::unique_ptr<StreamListener> listener_;
    // With workers: listener_ itself, and the TLS config each worker wraps connections in.
    TcpListener *tcp_listener_ = nullptr;
    std::shared_ptr<TlsConfig> tls_;
    std::vector<std::unique_ptr<CocoThread>> workers_;
    Acceptor *acceptor_ = nullptr;
    // Stop() has started, and has finished.
    bool stopping_ = false;
    bool stopped_ = false;
};

}  // namespace coco
