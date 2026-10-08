#include "coco/app/http/server.hpp"

#include <strings.h>

#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/log/log.hpp"
#include "coco/net/tcp.hpp"
#include "coco/net/tls/conn.hpp"
#include "coco/utils/bufio.hpp"

namespace coco {

// Answers a request that never reaches a handler, then the connection closes.
static void WriteBareError(StreamConn &conn, int code, const char *detail = nullptr) {
    std::string msg = detail ? detail : std::to_string(code) + " " + HttpStatusText(code);
    std::string rsp = "HTTP/1.1 " + std::to_string(code) + " " + HttpStatusText(code) +
                      "\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n"
                      "Content-Length: " +
                      std::to_string(msg.size()) + "\r\n\r\n" + msg;
    conn.Write((void *)rsp.data(), rsp.size(), nullptr);
}

class HttpServerConn {
 public:
    static int Serve(StreamConn &conn, HttpHandler *handler, const HttpServeOptions &opt);
};

int HttpServerConn::Serve(StreamConn &conn, HttpHandler *handler, const HttpServeOptions &opt) {
    conn.SetRecvTimeout(opt.read_timeout_us);
    conn.SetSendTimeout(opt.write_timeout_us);

    // One reader, writer and request for the whole connection: buffers are reused, and
    // bytes of a pipelined request read with the previous one are kept.
    BufReader br(&conn);
    HttpResponseWriter w(&conn, &br);
    HttpRequest req;
    std::string remote = conn.RemoteAddr();

    while (!CocoShouldStop()) {
        int ret = ReadHttpRequest(&br, opt.max_header_bytes, &req);
        if (ret == ERROR_HTTP_HEADER_TOO_LARGE) {
            WriteBareError(conn, HttpStatusRequestHeaderFieldsTooLarge);
            return ret;
        }
        if (ret == ERROR_HTTP_PARSE_HEADER || ret == ERROR_HTTP_PARSE_URI) {
            WriteBareError(conn, HttpStatusBadRequest);
            return ret;
        }
        if (ret != COCO_SUCCESS) {
            if (!coco_is_client_gracefully_close(ret) && ret != ERROR_THREAD_INTERRUPED) {
                coco_warn("http: read request failed. ret=%d", ret);
            }
            return ret;
        }
        req.remote_addr = remote;

        // RFC 7230 5.4.
        if (req.ProtoAtLeast(1, 1) && !req.header.Has(HttpHeaderHost)) {
            WriteBareError(conn, HttpStatusBadRequest,
                           "400 Bad Request: missing required Host header");
            return ERROR_HTTP_PARSE_HEADER;
        }
        const std::string &expect = req.header.Get(HttpHeaderExpect);
        if (!expect.empty()) {
            if (strcasecmp(expect.c_str(), "100-continue") != 0 || !req.ProtoAtLeast(1, 1)) {
                WriteBareError(conn, HttpStatusExpectationFailed);
                return ERROR_HTTP_PARSE_HEADER;
            }
            if (!req.body.Eof()) {
                req.body.SetContinue(&conn);
            }
        }

        w.Reset(&req);
        handler->ServeHTTP(w, req);
        if (w.Hijacked()) {
            return COCO_SUCCESS;
        }
        if ((ret = w.Finish()) != COCO_SUCCESS) {
            return ret;
        }
        if (w.ShouldClose()) {
            return COCO_SUCCESS;
        }
        // The next request starts after this body.
        if (!req.body.Eof() && req.body.Discard(opt.max_drain_bytes) != COCO_SUCCESS) {
            return COCO_SUCCESS;
        }
    }
    return COCO_SUCCESS;
}

int ServeHttpConn(StreamConn &conn, HttpHandler *handler, const HttpServeOptions &options) {
    return HttpServerConn::Serve(conn, handler, options);
}

HttpServer::HttpServer(HttpHandler *handler, HttpServeOptions options)
    : handler_(handler), options_(options) {}

HttpServer::HttpServer(HttpHandlerFunc handler, HttpServeOptions options)
    : owned_(std::make_shared<HttpFuncHandler>(std::move(handler))),
      handler_(owned_.get()),
      options_(options) {}

HttpServer::~HttpServer() { server_.reset(); }

int HttpServer::ListenAndServe(const std::string &ip, int port) {
    int ret = Start(ip, port);
    if (ret == COCO_SUCCESS) {
        Wait();
    }
    return ret;
}

int HttpServer::ListenAndServeTLS(const std::string &ip, int port, const std::string &crt_file,
                                  const std::string &key_file) {
    int ret = StartTLS(ip, port, crt_file, key_file);
    if (ret == COCO_SUCCESS) {
        Wait();
    }
    return ret;
}

int HttpServer::Serve(std::unique_ptr<StreamListener> l) {
    int ret = Start(std::move(l));
    if (ret == COCO_SUCCESS) {
        Wait();
    }
    return ret;
}

int HttpServer::Start(const std::string &ip, int port) {
    std::unique_ptr<TcpListener> l;
    int ret = ListenTcp(ip, port, &l);
    if (ret != COCO_SUCCESS) {
        coco_error("http: listen on %s:%d failed. ret=%d", ip.c_str(), port, ret);
        return ret;
    }
    return StartOn(std::move(l), "", "");
}

int HttpServer::StartTLS(const std::string &ip, int port, const std::string &crt_file,
                         const std::string &key_file) {
    std::unique_ptr<TcpListener> l;
    int ret = ListenTcp(ip, port, &l);
    if (ret != COCO_SUCCESS) {
        coco_error("https: listen on %s:%d failed. ret=%d", ip.c_str(), port, ret);
        return ret;
    }
    return StartOn(std::move(l), crt_file, key_file);
}

int HttpServer::Start(std::unique_ptr<StreamListener> l) { return StartOn(std::move(l), "", ""); }

void HttpServer::Wait() {
    if (server_) {
        server_->Wait();
    }
}

int HttpServer::StartOn(std::unique_ptr<StreamListener> l, const std::string &crt_file,
                        const std::string &key_file) {
    if (server_ != nullptr) {
        coco_error("http server already serving");
        return ERROR_THREAD_STARTED;
    }

    TcpServerOptions opt;
    // They also bound the TLS handshake, which runs before ServeHttpConn sets its own.
    opt.recv_timeout_us = options_.read_timeout_us;
    opt.send_timeout_us = options_.write_timeout_us;
    opt.threads = options_.threads;

    HttpHandler *handler = handler_;
    HttpServeOptions options = options_;
    StreamHandler serve = [handler, options](StreamConn &conn) {
        return ServeHttpConn(conn, handler, options);
    };
    if (!crt_file.empty() && !key_file.empty()) {
        std::shared_ptr<TlsConfig> cfg;
        int ret = TlsConfig::NewServer(key_file, crt_file, &cfg);
        if (ret != COCO_SUCCESS) {
            coco_error("https: loading %s and %s failed. ret=%d", crt_file.c_str(),
                       key_file.c_str(), ret);
            return ret;
        }
        serve = TlsHandler(cfg, serve);
    }
    server_.reset(new TcpServer(serve, opt));
    return server_->Start(std::move(l));
}

void HttpServer::Stop() {
    if (server_) {
        server_->Stop();
    }
}

}  // namespace coco
