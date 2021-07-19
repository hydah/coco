#include "net/layer7/coco_http.hpp"

#include <assert.h>
#include <netdb.h>
#include <string.h>
#include <algorithm>

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "net/coco_socket.hpp"

/* HttpServerConn */
HttpServerConn::HttpServerConn(ConnManager *mgr, TcpConn *conn, HttpServeMux *mux)
    : ConnRoutine(mgr) {
    conn_ = conn;
    _mux = mux;
    https_ = false;
}

HttpServerConn::HttpServerConn(ConnManager *mgr, SslServer *conn, HttpServeMux *mux)
    : ConnRoutine(mgr) {
    conn_ = conn;
    _mux = mux;
    https_ = true;
}

HttpServerConn::~HttpServerConn() {
    coco_info("destruct httpserver conn");
    coco_freep(conn_);
    coco_freep(http_msg_);
}

int HttpServerConn::ProcessRequest(HttpResponseWriter *w, HttpMessage *r) {
    int ret = COCO_SUCCESS;

    coco_trace("HTTP %s %s, content-length=%ld", r->method_str().c_str(), r->url().c_str(),
               r->content_length());

    // use default server mux to serve http request.
    if ((ret = _mux->serve_http(w, r)) != COCO_SUCCESS) {
        if (!coco_is_client_gracefully_close(ret)) {
            coco_error("serve http msg failed. ret=%d", ret);
        }
    }

    return ret;
}

int HttpServerConn::DoCycle() {
    int ret = COCO_SUCCESS;

    conn_->SetRecvTimeout(HTTP_RECV_TIMEOUT_US);

    if (https_) {
        // ssl handshake
        SslServer *ssl = reinterpret_cast<SslServer *>(conn_);
        ret = ssl->Handshake("./server.key", "./server.crt");
        if (ret != COCO_SUCCESS) {
            coco_error("ssl handshake failed");
            return ret;
        }
    }

    // process http messages.
    while (!ShouldTermCycle()) {
        // coco_freep(http_msg_);
        if (http_msg_ != nullptr) {
            delete http_msg_;
        }
        http_msg_ = new HttpMessage();

        // initialize parser
        if ((ret = http_msg_->Initialize(HTTP_REQUEST)) != COCO_SUCCESS) {
            coco_error("api initialize http parser failed. ret=%d", ret);
            return ret;
        }
        // get a http message
        if ((ret = http_msg_->Parse(conn_, this)) != COCO_SUCCESS) {
            return ret;
        }

        // ok, handle http request.
        HttpResponseWriter writer(conn_);
        if ((ret = ProcessRequest(&writer, http_msg_)) != COCO_SUCCESS) {
            return ret;
        }

        // read all rest bytes in request body.
        char buf[HTTP_READ_CACHE_BYTES];
        HttpResponseReader *br = http_msg_->body_reader();
        while (!br->eof()) {
            if ((ret = br->Read(buf, HTTP_READ_CACHE_BYTES, nullptr)) != COCO_SUCCESS) {
                return ret;
            }
        }

        // donot keep alive, disconnect it.
        if (!http_msg_->is_keep_alive()) {
            break;
        }
    }

    return ret;
}

/* HttpServer */
HttpServer::HttpServer(bool https) {
    _l = nullptr;
    _mux = nullptr;
    https_ = https;
    manager = new ConnManager();
}

HttpServer::~HttpServer() {
    if (_l) {
        delete _l;
        _l = nullptr;
    }
    if (manager) {
        delete manager;
        manager = nullptr;
    }
}

int HttpServer::ListenAndServe(std::string local_ip, int local_port, HttpServeMux *mux) {
    _l = ListenTcp(local_ip, local_port);
    if (_l == nullptr) {
        coco_error("create http listen socket failed");
        return -1;
    }
    _mux = mux;
    return 0;
}

int HttpServer::Serve(TcpListener *l, HttpServeMux *mux) {
    _l = l;
    _mux = mux;

    return 0;
}

int HttpServer::Cycle() {
    while (true) {
        manager->Destroy();

        TcpConn *conn_ = _l->Accept();
        if (conn_ == nullptr) {
            coco_error("get null conn");
            continue;
        }
        HttpServerConn *conn = nullptr;
        if (https_) {
            auto ssl = new SslServer(conn_->GetStfd(), conn_);
            conn = new HttpServerConn(manager, ssl, _mux);
        } else {
            conn = new HttpServerConn(manager, conn_, _mux);
        }

        conn->Start();
    }
    return 0;
}

HttpClient::~HttpClient() {
    Disconnect();
    coco_freep(http_msg_);
}

int HttpClient::Initialize(bool is_https, std::string _h, int p, int64_t t_us) {
    int ret = COCO_SUCCESS;

    coco_freep(http_msg_);
    http_msg_ = new HttpMessage();
    if ((ret = http_msg_->Initialize(HTTP_RESPONSE)) != COCO_SUCCESS) {
        coco_error("initialize parser failed. ret=%d", ret);
        return ret;
    }

    host_ = _h;
    port_ = p;
    timeout_us_ = HTTP_CLIENT_TIMEOUT_US;

    is_https_ = is_https;
    // we just handle the default port when https
    if ((is_https_) && (80 == port_)) {
        port_ = 443;
    }
    return ret;
}

int HttpClient::Post(std::string path, std::string req, HttpMessage **ppmsg,
                     std::string request_id) {
    *ppmsg = nullptr;

    int ret = COCO_SUCCESS;

    if ((ret = Connect()) != COCO_SUCCESS) {
        coco_warn("http post. connect server failed. [host:%s, port:%d]ret=%d", host_.c_str(),
                  port_, ret);
        return ret;
    }

    // send POST request to uri
    // POST %s HTTP/1.1\r\nHost: %s\r\nContent-Length: %d\r\n\r\n%s
    std::stringstream ss;
    ss << "POST " << path << " "
       << "HTTP/1.1" << HTTP_CRLF << "Host: " << host_ << HTTP_CRLF << "Request-Id: " << request_id
       << HTTP_CRLF << "Connection: Keep-Alive" << HTTP_CRLF << "Content-Length: " << std::dec
       << req.length() << HTTP_CRLF << "User-Agent: "
       << "coco" << HTTP_CRLF << "Content-Type: application/json" << HTTP_CRLF << HTTP_CRLF << req;

    std::string data = ss.str();
    if ((ret = conn_->Write((void *)data.c_str(), data.length(), nullptr)) != COCO_SUCCESS) {
        // disconnect when error.
        Disconnect();

        coco_error("http post. write failed. ret=%d", ret);
        return ret;
    }

    if ((ret = http_msg_->Parse(conn_, nullptr)) != COCO_SUCCESS) {
        coco_error("http post. parse response failed. ret=%d", ret);
        return ret;
    }
    coco_info("http post. parse response success.");
    *ppmsg = http_msg_;
    return ret;
}

int HttpClient::Get(std::string path, std::string req, HttpMessage **ppmsg,
                    std::string request_id) {
    *ppmsg = nullptr;

    int ret = COCO_SUCCESS;

    if ((ret = Connect()) != COCO_SUCCESS) {
        coco_warn("http connect server failed. ret=%d", ret);
        return ret;
    }

    // send POST request to uri
    // GET %s HTTP/1.1\r\nHost: %s\r\nContent-Length: %d\r\n\r\n%s
    std::stringstream ss;
    ss << "GET " << path << " "
       << "HTTP/1.1" << HTTP_CRLF << "Host: " << host_ << HTTP_CRLF << "Request-Id: " << request_id
       << HTTP_CRLF << "Connection: Keep-Alive" << HTTP_CRLF << "Content-Length: " << std::dec
       << req.length() << HTTP_CRLF << "User-Agent: "
       << "coco" << HTTP_CRLF << "Content-Type: application/json" << HTTP_CRLF << HTTP_CRLF << req;

    std::string data = ss.str();
    if ((ret = conn_->Write((void *)data.c_str(), data.length(), nullptr)) != COCO_SUCCESS) {
        // disconnect when error.
        Disconnect();
        coco_error("write http get failed. ret=%d", ret);
        return ret;
    }

    if ((ret = http_msg_->Parse(conn_, nullptr)) != COCO_SUCCESS) {
        coco_error("parse http post response failed. ret=%d", ret);
        return ret;
    }
    coco_info("parse http get response success.");
    *ppmsg = http_msg_;

    return ret;
}

void HttpClient::Disconnect() {
    connected_ = false;
    coco_freep(conn_);
}

int HttpClient::Connect() {
    int ret = COCO_SUCCESS;

    if (connected_) {
        return ret;
    }

    Disconnect();

    // open socket.
    auto conn = DialTcp(host_, port_, (int)timeout_us_);
    if (conn == nullptr) {
        coco_warn("http client failed, server=%s, port=%d, timeout=%ld", host_.c_str(), port_,
                  timeout_us_);
        return -1;
    }
    coco_info("connect to server success. server=%s, port=%d", host_.c_str(), port_);

    if (is_https_) {
        auto ssl = new SslClient(conn->GetStfd(), conn);
        conn_ = ssl;
        ret = ssl->Handshake();
        if (ret != COCO_SUCCESS) {
            coco_error("ssl handshake failed");
            return ret;
        }
    } else {
        conn_ = conn;
    }

    conn_->SetRecvTimeout(timeout_us_);
    conn_->SetSendTimeout(timeout_us_);
    connected_ = true;

    return ret;
}