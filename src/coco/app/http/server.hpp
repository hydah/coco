#pragma once
#include <stdint.h>

#include <memory>
#include <string>

#include "coco/app/http/codec/basic.hpp"
#include "coco/app/http/handler.hpp"
#include "coco/net/conn.hpp"
#include "coco/net/tcp_server.hpp"

namespace coco {

struct HttpServeOptions {
    // Bounds every read, including the wait for the next request on an idle connection.
    int64_t read_timeout_us = HTTP_RECV_TIMEOUT_US;
    int64_t write_timeout_us = HTTP_RECV_TIMEOUT_US;
    // Request line plus header fields; larger requests get 431. http-parser caps it at 80KB.
    size_t max_header_bytes = 64 * 1024;
    // A request body the handler left unread is skipped up to this size to keep the
    // connection; a longer one closes it.
    int64_t max_drain_bytes = 256 * 1024;
    // Only for HttpServer: above 1, connections are served on this many worker threads,
    // see TcpServerOptions::threads. The handler is then called on several threads at
    // once.
    int threads = 0;
};

// Serves HTTP/1.1 requests on conn with handler until the peer closes, a response ends
// the connection, the handler hijacks it, or the coroutine is stopped. conn may be plain
// TCP or TLS. Pipelined requests are answered in order. Like Go's http (*conn).serve.
int ServeHttpConn(StreamConn &conn, HttpHandler *handler,
                  const HttpServeOptions &options = HttpServeOptions());

// An HTTP/1.1 server: a TcpServer whose handler is ServeHttpConn, like Go's http.Server.
//
//   HttpServeMux mux;
//   mux.HandleFunc("GET /hello/{name}", [](HttpResponseWriter &w, HttpRequest &r) {
//       w.Write("hello " + r.PathValue("name"));
//   });
//   HttpServer server(&mux);
//   return server.ListenAndServe("0.0.0.0", 8080);  // until Ctrl-C
//
// To run several servers, Start() each of them and then CocoWaitForShutdown().
class HttpServer {
 public:
    // handler, and everything it uses, must outlive the server.
    explicit HttpServer(HttpHandler *handler, HttpServeOptions options = HttpServeOptions());
    explicit HttpServer(HttpHandlerFunc handler, HttpServeOptions options = HttpServeOptions());
    // Stops the server; the connections exit before the handler they use may go away.
    virtual ~HttpServer();

    // Listen on ip:port, over TLS with a PEM certificate chain and private key, or take
    // ownership of l, and serve until Stop() or a shutdown (CocoShutdown(), SIGINT,
    // SIGTERM), which stops the server before they return COCO_SUCCESS.
    int ListenAndServe(const std::string &ip, int port);
    int ListenAndServeTLS(const std::string &ip, int port, const std::string &crt_file,
                          const std::string &key_file);
    int Serve(std::unique_ptr<StreamListener> l);

    // The same, but return once serving has started.
    int Start(const std::string &ip, int port);
    int StartTLS(const std::string &ip, int port, const std::string &crt_file,
                 const std::string &key_file);
    int Start(std::unique_ptr<StreamListener> l);
    // Blocks until Stop() or a shutdown; a shutdown stops the server first.
    void Wait();

    // Must not be called from a handler, which may call CocoShutdown() instead.
    void Stop();

 private:
    int StartOn(std::unique_ptr<StreamListener> l, const std::string &crt_file,
                const std::string &key_file);

    std::shared_ptr<HttpHandler> owned_;
    HttpHandler *handler_;
    HttpServeOptions options_;
    std::unique_ptr<TcpServer> server_;
};

}  // namespace coco
