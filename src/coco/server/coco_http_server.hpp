#pragma once

#include <memory>
#include <string>

#include "coco/net/layer7/http/coco_http.hpp"
#include "coco/server/coco_tcp_server.hpp"

namespace coco {

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
