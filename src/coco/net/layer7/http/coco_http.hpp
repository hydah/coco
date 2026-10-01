#pragma once
#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "coco/net/layer4/coco_layer4.hpp"
#include "coco/net/layer7/http/http_basic.h"
#include "coco/net/layer7/http/http_message.h"
#include "coco/net/layer7/http/http_mux.h"
#include "coco/net/layer7/http/http_writer.h"

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
};

// Serves HTTP/1.1 requests on conn with handler until the peer closes, a response ends
// the connection, the handler hijacks it, or the coroutine is stopped. conn may be plain
// TCP or TLS. Pipelined requests are answered in order.
int ServeHttpConn(StreamConn &conn, HttpHandler *handler,
                  const HttpServeOptions &options = HttpServeOptions());

#define HTTP_CLIENT_TIMEOUT_US (int64_t)(30 * 1000 * 1000LL)

struct HttpUrl;

class HttpClientConn {
 public:
    HttpClientConn(const std::string &key, std::unique_ptr<StreamConn> conn)
        : key(key), conn(std::move(conn)), br(this->conn.get()) {}

    // "scheme://host:port", the pool it may go back to.
    std::string key;
    std::unique_ptr<StreamConn> conn;
    BufReader br;
};

// Idle keep-alive connections by key, most recently used first out.
class HttpConnPool {
 public:
    std::unique_ptr<HttpClientConn> Get(const std::string &key);
    void Put(std::unique_ptr<HttpClientConn> c);
    void Clear() { idle_.clear(); }

    size_t max_idle_per_host = 2;

 private:
    std::map<std::string, std::vector<std::unique_ptr<HttpClientConn>>> idle_;
};

// An HTTP/1.1 client like Go's http.Client: keep-alive connections are pooled per host
// and reused, redirects are followed. A request on a pooled connection the server closed
// meanwhile is retried once on a new connection when that is safe.
//
//   HttpClient client;
//   std::unique_ptr<HttpResponse> resp;
//   if (client.Get("http://127.0.0.1:8080/hello", &resp) == COCO_SUCCESS) {
//       std::string body;
//       resp->body.ReadAll(&body);
//   }
//
// Responses may outlive the client; their connections are then closed instead of pooled.
class HttpClient {
 public:
    explicit HttpClient(int64_t timeout_us = HTTP_CLIENT_TIMEOUT_US);
    ~HttpClient();

    HttpClient(const HttpClient &) = delete;
    HttpClient &operator=(const HttpClient &) = delete;

    // http:// connects with dialer, TcpDialer() by default.
    void SetDialer(StreamDialer dialer) { dialer_ = dialer; }
    // https:// connects with dialer, e.g. TlsDialer() from net/tls; without one https
    // requests fail with ERROR_HTTPS_NOT_SUPPORTED.
    void SetTlsDialer(StreamDialer dialer) { tls_dialer_ = dialer; }

    // Sends req and reads the response header; read the body from (*resp)->body. A non-2xx
    // status is not an error.
    int Do(HttpRequest &req, std::unique_ptr<HttpResponse> *resp);
    int Get(const std::string &url, std::unique_ptr<HttpResponse> *resp);
    int Post(const std::string &url, const std::string &content_type, const std::string &body,
             std::unique_ptr<HttpResponse> *resp);
    void CloseIdleConnections() { pool_->Clear(); }

    // Bounds the connect and every read and write.
    int64_t timeout_us;
    size_t max_idle_conns_per_host = 2;
    // 0 returns redirects to the caller.
    int max_redirects = 10;
    size_t max_header_bytes = 64 * 1024;

 private:
    int RoundTrip(HttpRequest &req, const HttpUrl &u, std::unique_ptr<HttpResponse> *resp);

    StreamDialer dialer_;
    StreamDialer tls_dialer_;
    std::shared_ptr<HttpConnPool> pool_;
};

// A client shared by HttpGet and HttpPost, like Go's http.DefaultClient. Call
// SetTlsDialer on it to enable https.
HttpClient &HttpDefaultClient();
int HttpGet(const std::string &url, std::unique_ptr<HttpResponse> *resp);
int HttpPost(const std::string &url, const std::string &content_type, const std::string &body,
             std::unique_ptr<HttpResponse> *resp);

}  // namespace coco
