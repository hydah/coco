#pragma once

#include <memory>
#include <string>

#include "net/layer7/http/coco_http.hpp"
#include "server/coco_tcp_server.hpp"

// An HTTP/1.1 server: a TcpServer whose handler is ServeHttpConn, like Go's http.Server.
//
//   HttpServeMux mux;
//   mux.HandleFunc("GET /hello/{name}", [](HttpResponseWriter &w, HttpRequest &r) {
//       w.Write("hello " + r.PathValue("name"));
//   });
//   HttpServer server(&mux);
//   server.ListenAndServe("0.0.0.0", 8080);
class HttpServer {
 public:
    // handler, and everything it uses, must outlive the server.
    explicit HttpServer(HttpHandler *handler, HttpServeOptions options = HttpServeOptions());
    explicit HttpServer(HttpHandlerFunc handler, HttpServeOptions options = HttpServeOptions());
    // Stops the server; the connections exit before the handler they use may go away.
    virtual ~HttpServer();

    // Listens on ip:port and starts serving; returns once serving has started.
    int ListenAndServe(const std::string &ip, int port);
    // The same over TLS, with a PEM certificate chain and private key.
    int ListenAndServeTLS(const std::string &ip, int port, const std::string &crt_file,
                          const std::string &key_file);
    // Takes ownership of l and starts serving on it.
    int Serve(std::unique_ptr<StreamListener> l);
    void Stop();

 private:
    int Start(std::unique_ptr<StreamListener> l, const std::string &crt_file,
              const std::string &key_file);

    std::shared_ptr<HttpHandler> owned_;
    HttpHandler *handler_;
    HttpServeOptions options_;
    std::unique_ptr<TcpServer> server_;
};
