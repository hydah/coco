#include "net/layer7/http/coco_http.hpp"

#include <assert.h>
#include <netdb.h>
#include <string.h>
#include <algorithm>

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "net/coco_socket.hpp"

static int ProcessRequest(HttpServeMux *mux, HttpResponseWriter *w, HttpMessage *r) {
    int ret = COCO_SUCCESS;

    coco_trace("HTTP %s %s, content-length=%lld", r->method_str().c_str(), r->url().c_str(),
               (long long)r->content_length());

    if ((ret = mux->serve_http(w, r)) != COCO_SUCCESS) {
        if (!coco_is_client_gracefully_close(ret)) {
            coco_error("serve http msg failed. ret=%d", ret);
        }
    }

    return ret;
}

int ServeHttpConn(StreamConn &conn, HttpServeMux *mux) {
    int ret = COCO_SUCCESS;

    conn.SetRecvTimeout(HTTP_RECV_TIMEOUT_US);

    // process http messages.
    while (!CocoShouldStop()) {
        std::unique_ptr<HttpMessage> msg(new HttpMessage());

        // initialize parser
        if ((ret = msg->Initialize(HTTP_REQUEST)) != COCO_SUCCESS) {
            coco_error("api initialize http parser failed. ret=%d", ret);
            return ret;
        }
        // get a http message
        if ((ret = msg->Parse(&conn, &conn)) != COCO_SUCCESS) {
            return ret;
        }

        // ok, handle http request.
        HttpResponseWriter writer(&conn);
        if ((ret = ProcessRequest(mux, &writer, msg.get())) != COCO_SUCCESS) {
            return ret;
        }

        // read all rest bytes in request body. A request with neither Content-Length nor
        // chunked encoding has no body; the reader would otherwise read until the peer closes.
        char buf[HTTP_READ_CACHE_BYTES];
        HttpResponseReader *br = msg->body_reader();
        bool has_body = msg->is_chunked() || msg->content_length() > 0;
        while (has_body && !br->eof()) {
            if ((ret = br->Read(buf, HTTP_READ_CACHE_BYTES, nullptr)) != COCO_SUCCESS) {
                return ret;
            }
        }

        // donot keep alive, disconnect it.
        if (!msg->is_keep_alive()) {
            break;
        }
    }

    return ret;
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
    timeout_us_ = t_us;

    is_https_ = is_https;
    // we just handle the default port when https
    if ((is_https_) && (80 == port_)) {
        port_ = 443;
    }
    method_ = "GET";

    return ret;
}

bool HttpClient::SetMethod(std::string method) {
    method_ = method;
    return true;
}
bool HttpClient::SetHeader(std::string key, std::string value) {
    http_header_.set(key, value);
    return true;
}

int HttpClient::SendRequest() {
    int ret = COCO_SUCCESS;

    if ((ret = Connect()) != COCO_SUCCESS) {
        coco_warn("http %s. connect server failed. [host:%s, port:%d]ret=%d", method_.c_str(),
                  host_.c_str(), port_, ret);
        return ret;
    }

    std::stringstream ss;
    ss << method_ << " " << path_ << " "
       << "HTTP/1.1" << HTTP_CRLF << http_header_.Encode() << HTTP_CRLF;
    if (!req_.empty()) {
        ss << req_;
    }

    std::string data = ss.str();
    if ((ret = conn_->Write((void *)data.c_str(), data.length(), nullptr)) != COCO_SUCCESS) {
        // disconnect when error.
        Disconnect();
        coco_error("write http get failed. ret=%d", ret);
        return ret;
    }

    if ((ret = http_msg_->Parse(conn_, nullptr)) != COCO_SUCCESS) {
        coco_error("http post. parse response failed. ret=%d", ret);
        return ret;
    }

    return ret;
}

int HttpClient::Post(std::string path, std::string req, HttpMessage **ppmsg,
                     std::string request_id) {
    int ret = COCO_SUCCESS;

    method_ = "POST";
    path_ = path;
    req_ = req;
    *ppmsg = nullptr;

    SetHeader("Host", host_);
    SetHeader("Request-Id", request_id);
    SetHeader("Connection", "Keep-Alive");
    SetHeader("Content-Length", std::to_string(req.length()));
    SetHeader("User-Agent", "coco");
    SetHeader("Content-Type", "application/json");

    if ((ret = SendRequest()) != COCO_SUCCESS) {
        return ret;
    }

    coco_info("http post. parse response success.");
    *ppmsg = http_msg_;
    return ret;
}

int HttpClient::Get(std::string path, std::string req, HttpMessage **ppmsg,
                    std::string request_id) {
    int ret = COCO_SUCCESS;

    method_ = "GET";
    path_ = path;
    req_ = req;
    *ppmsg = nullptr;

    SetHeader("Host", host_);
    SetHeader("Request-Id", request_id);
    SetHeader("Connection", "Keep-Alive");
    SetHeader("Content-Length", std::to_string(req.length()));
    SetHeader("User-Agent", "coco");
    SetHeader("Content-Type", "application/json");

    if ((ret = SendRequest()) != COCO_SUCCESS) {
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
        coco_warn("http client failed, server=%s, port=%d, timeout=%lld", host_.c_str(), port_,
                  (long long)timeout_us_);
        return -1;
    }
    coco_info("connect to server success. server=%s, port=%d", host_.c_str(), port_);

    if (is_https_) {
        auto ssl = new SslClient(conn->GetStfd(), conn);
        conn_ = ssl;
        // The handshake does socket IO too, so it needs the timeouts.
        conn_->SetRecvTimeout(timeout_us_);
        conn_->SetSendTimeout(timeout_us_);
        ret = ssl->Handshake();
        if (ret != COCO_SUCCESS) {
            coco_error("ssl handshake failed");
            return ret;
        }
    } else {
        conn_ = conn;
        conn_->SetRecvTimeout(timeout_us_);
        conn_->SetSendTimeout(timeout_us_);
    }

    connected_ = true;

    return ret;
}

StreamConn *HttpClient::GetUnderlayerConn() { return conn_; }