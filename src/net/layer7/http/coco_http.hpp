#pragma once
#include <sstream>

#include "http-parser/http_parser.h"

#include "net/tls/coco_ssl.hpp"
#include "net/layer4/coco_tcp.hpp"
#include "net/layer7/http/http_io.h"
#include "net/layer7/http/http_message.h"
#include "net/layer7/http/http_mux.h"
#include "utils/utils.hpp"

// Serves HTTP/1.1 requests on conn through mux until the peer closes, a request is not
// keep-alive, or the coroutine is stopped. conn may be plain TCP or already-handshaken TLS.
// The parsed message's observer is &conn. An Upgrade request is the last one: its handler
// may take over conn (e.g. WebSocketHandler) and returns when the connection is done.
int ServeHttpConn(StreamConn &conn, HttpServeMux *mux);

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