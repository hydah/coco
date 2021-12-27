#pragma once
#include <sstream>

#include "http-parser/http_parser.h"

#include "net/layer4/coco_ssl.hpp"
#include "net/layer4/coco_tcp.hpp"
#include "protocol/http/http_io.h"
#include "protocol/http/http_message.h"
#include "protocol/http/http_mux.h"
#include "utils/utils.hpp"

class HttpServerConn : public ConnRoutine {
 public:
    HttpServerConn(ConnManager *manager, TcpConn *conn, HttpServeMux *mux);
    HttpServerConn(ConnManager *manager, SslServer *conn, HttpServeMux *mux);
    virtual ~HttpServerConn();

 public:
    virtual int DoCycle();
    int ProcessRequest(HttpResponseWriter *w, HttpMessage *r);
    virtual std::string GetRemoteAddr() { return conn_->RemoteAddr(); };

 private:
    StreamConn *conn_ = nullptr;
    HttpServeMux *_mux = nullptr;
    HttpMessage *http_msg_ = nullptr;
    bool https_ = false;
};

class HttpServer : public ListenRoutine {
 public:
    HttpServer(bool https);
    virtual ~HttpServer();

    virtual int ListenAndServe(std::string local_ip, int local_port, HttpServeMux *mux);
    virtual int Serve(TcpListener *l, HttpServeMux *mux);
    virtual int Cycle();

 private:
    TcpListener *_l;
    HttpServeMux *_mux;
    ConnManager *manager;
    bool https_ = false;
};

// the default timeout for http client. 1s
#define HTTP_CLIENT_TIMEOUT_US (int64_t)(1 * 1000 * 1000LL)

/**
 * http client to GET/POST/PUT/DELETE uri
 */
class HttpClient {
 public:
    HttpClient() = default;
    virtual ~HttpClient();

    /**
     * initialize the client, connect to host and port.
     */
    int Initialize(bool is_https, std::string h, int p, int64_t t_us = HTTP_CLIENT_TIMEOUT_US);
    bool SetMethod(std::string method);
    bool SetHeader(std::string key, std::string value);
    virtual int SendRequest();
    virtual void Disconnect();
    virtual int Connect();
    void SetPath(std::string path) { path_ = path; };
    HttpMessage *GetHttpMessage() { return http_msg_; };
    StreamConn *GetUnderlayerConn();

    /**
     * to post data to the uri.
     * @param the path to request on.
     * @param req the data post to uri. empty string to ignore.
     * @param ppmsg output the http message to read the response.
     * @param request_id request id, used for trace log.
     */
    virtual int Post(std::string path, std::string req, HttpMessage **ppmsg,
                     std::string request_id = "");
    /**
     * to get data from the uri.
     * @param the path to request on.
     * @param req the data post to uri. empty string to ignore.
     * @param ppmsg output the http message to read the response.
     * @param request_id request id, used for trace log.
     */
    virtual int Get(std::string path, std::string req, HttpMessage **ppmsg,
                    std::string request_id = "");

 private:
    std::string method_;
    HttpHeader http_header_;
    StreamConn *conn_ = nullptr;
    HttpMessage *http_msg_ = nullptr;
    bool connected_ = false;
    bool is_https_ = false;
    int64_t timeout_us_;
    // host name or ip.
    std::string host_ = "";
    int port_ = -1;
    std::string path_;
    std::string req_;
};