#pragma once

#include <string>

#include "net/layer7/http/coco_http.hpp"
#include "server/coco_tcp_server.hpp"

// An HTTP/1.1 server: a TcpServer whose handler is ServeHttpConn. With https, every
// connection is handshaken with ./server.key and ./server.crt first.
class HttpServer {
 public:
    explicit HttpServer(bool https);
    // Stops the server; the connections exit before the mux they serve with may go away.
    virtual ~HttpServer();

    // Listens on ip:port and starts serving through mux, which must outlive the server.
    virtual int ListenAndServe(std::string local_ip, int local_port, HttpServeMux *mux);
    // Takes ownership of l and starts serving through mux.
    virtual int Serve(TcpListener *l, HttpServeMux *mux);
    virtual void Stop();

 private:
    TcpServerOptions Options() const;

    TcpServer *server_ = nullptr;
    bool https_ = false;
};
