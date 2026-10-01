#include "net/layer7/http/coco_http.hpp"

#include <string.h>
#include <strings.h>
#include <sys/uio.h>

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "net/layer4/coco_tcp.hpp"

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
            WriteBareError(conn, 431);
            return ret;
        }
        if (ret == ERROR_HTTP_PARSE_HEADER || ret == ERROR_HTTP_PARSE_URI) {
            WriteBareError(conn, 400);
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
        if (req.ProtoAtLeast(1, 1) && !req.header.Has("Host")) {
            WriteBareError(conn, 400, "400 Bad Request: missing required Host header");
            return ERROR_HTTP_PARSE_HEADER;
        }
        const std::string &expect = req.header.Get("Expect");
        if (!expect.empty()) {
            if (strcasecmp(expect.c_str(), "100-continue") != 0 || !req.ProtoAtLeast(1, 1)) {
                WriteBareError(conn, 417);
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

std::unique_ptr<HttpClientConn> HttpConnPool::Get(const std::string &key) {
    auto it = idle_.find(key);
    if (it == idle_.end() || it->second.empty()) {
        return nullptr;
    }
    std::unique_ptr<HttpClientConn> c = std::move(it->second.back());
    it->second.pop_back();
    return c;
}

void HttpConnPool::Put(std::unique_ptr<HttpClientConn> c) {
    std::vector<std::unique_ptr<HttpClientConn>> &list = idle_[c->key];
    if (list.size() < max_idle_per_host) {
        list.push_back(std::move(c));
    }
}

HttpResponse::HttpResponse() = default;

HttpResponse::~HttpResponse() {
    if (!conn_ || close || status_code == 101 || !body.DiscardBuffered()) {
        return;
    }
    std::shared_ptr<HttpConnPool> pool = pool_.lock();
    if (pool) {
        pool->Put(std::move(conn_));
    }
}

StreamConn *HttpResponse::Conn() { return conn_ ? conn_->conn.get() : nullptr; }

BufReader *HttpResponse::Reader() { return conn_ ? &conn_->br : nullptr; }

void HttpResponse::Close() { conn_.reset(); }

struct HttpUrl {
    bool tls = false;
    std::string host;
    int port = 80;
    // path and query, as sent on the request line.
    std::string request_uri;
    // host, with brackets for IPv6 and the port when it is not the default.
    std::string host_header;
    // "scheme://host:port", what pooled connections are keyed by.
    std::string key;
};

static int ParseClientUrl(const std::string &url, HttpUrl *u) {
    http_parser_url pu;
    http_parser_url_init(&pu);
    if (http_parser_parse_url(url.data(), url.size(), 0, &pu) != 0) {
        coco_error("http: bad url %s", url.c_str());
        return ERROR_HTTP_PARSE_URI;
    }
    auto field = [&](http_parser_url_fields f) {
        return (pu.field_set & (1 << f)) ? url.substr(pu.field_data[f].off, pu.field_data[f].len)
                                         : std::string();
    };
    std::string scheme = field(UF_SCHEMA);
    u->tls = strcasecmp(scheme.c_str(), "https") == 0;
    u->host = field(UF_HOST);
    if ((!u->tls && strcasecmp(scheme.c_str(), "http") != 0) || u->host.empty()) {
        coco_error("http: url must be http://host or https://host, got %s", url.c_str());
        return ERROR_HTTP_PARSE_URI;
    }
    int def = u->tls ? 443 : 80;
    u->port = pu.port != 0 ? pu.port : def;
    u->request_uri = field(UF_PATH);
    if (u->request_uri.empty()) {
        u->request_uri = "/";
    }
    if (pu.field_set & (1 << UF_QUERY)) {
        u->request_uri += "?" + field(UF_QUERY);
    }
    // RFC 7230 5.4: an IPv6 literal goes in brackets, and a non-default port is included.
    u->host_header = u->host.find(':') != std::string::npos ? "[" + u->host + "]" : u->host;
    if (u->port != def) {
        u->host_header += ":" + std::to_string(u->port);
    }
    u->key = (u->tls ? "https://" : "http://") + u->host + ":" + std::to_string(u->port);
    return COCO_SUCCESS;
}

// Resolves a Location header against the URL that answered with it.
static std::string ResolveLocation(const HttpUrl &base, const std::string &loc) {
    if (loc.find("://") != std::string::npos) {
        return loc;
    }
    std::string scheme = base.tls ? "https:" : "http:";
    if (loc.compare(0, 2, "//") == 0) {
        return scheme + loc;
    }
    std::string origin = scheme + "//" + base.host_header;
    if (!loc.empty() && loc[0] == '/') {
        return origin + loc;
    }
    std::string path = base.request_uri.substr(0, base.request_uri.find('?'));
    return origin + path.substr(0, path.rfind('/') + 1) + loc;
}

HttpClient::HttpClient(int64_t timeout_us)
    : timeout_us(timeout_us), dialer_(TcpDialer()), pool_(std::make_shared<HttpConnPool>()) {}

HttpClient::~HttpClient() { pool_->Clear(); }

int HttpClient::Get(const std::string &url, std::unique_ptr<HttpResponse> *resp) {
    HttpRequest req("GET", url);
    return Do(req, resp);
}

int HttpClient::Post(const std::string &url, const std::string &content_type,
                     const std::string &body, std::unique_ptr<HttpResponse> *resp) {
    HttpRequest req("POST", url, body);
    req.header.Set("Content-Type", content_type);
    return Do(req, resp);
}

int HttpClient::Do(HttpRequest &req, std::unique_ptr<HttpResponse> *resp) {
    resp->reset();
    HttpUrl u;
    int ret = ParseClientUrl(req.url, &u);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    pool_->max_idle_per_host = max_idle_conns_per_host;

    HttpRequest *cur = &req;
    std::unique_ptr<HttpRequest> redirected;
    for (int redirects = 0;; ++redirects) {
        if ((ret = RoundTrip(*cur, u, resp)) != COCO_SUCCESS) {
            return ret;
        }
        int code = (*resp)->status_code;
        const std::string &loc = (*resp)->header.Get("Location");
        bool redirect = code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
        if (!redirect || max_redirects <= 0 || loc.empty()) {
            return COCO_SUCCESS;
        }
        if (redirects >= max_redirects) {
            coco_warn("http: stopped after %d redirects", redirects);
            return ERROR_HTTP_TOO_MANY_REDIRECTS;
        }

        std::string target = ResolveLocation(u, loc);
        HttpUrl next;
        if (ParseClientUrl(target, &next) != COCO_SUCCESS) {
            return COCO_SUCCESS;
        }
        std::unique_ptr<HttpRequest> r(new HttpRequest(cur->method, target, cur->SendBody()));
        r->header = cur->header;
        r->close = cur->close;
        // Like browsers and Go: 301/302/303 turn into a GET without a body.
        if (code <= 303 && cur->method != "GET" && cur->method != "HEAD") {
            r->method = "GET";
            r->SetBody("");
            r->header.Del("Content-Type");
            r->header.Del("Content-Length");
        }
        if (next.host != u.host) {
            r->header.Del("Authorization");
            r->header.Del("Cookie");
            r->header.Del("WWW-Authenticate");
        }
        // Frees the old response; its connection goes back to the pool if possible.
        resp->reset();
        redirected = std::move(r);
        cur = redirected.get();
        u = next;
    }
}

int HttpClient::RoundTrip(HttpRequest &req, const HttpUrl &u, std::unique_ptr<HttpResponse> *out) {
    const StreamDialer &dial = u.tls ? tls_dialer_ : dialer_;
    if (!dial) {
        coco_error("http: %s needs SetTlsDialer()", req.url.c_str());
        return ERROR_HTTPS_NOT_SUPPORTED;
    }

    const std::string &body = req.SendBody();
    const HttpHeader &h = req.header;
    std::string head;
    head.reserve(256);
    head.append(req.method).append(" ", 1).append(u.request_uri).append(" HTTP/1.1\r\n", 11);
    if (!h.Has("Host")) {
        head.append("Host: ").append(req.host.empty() ? u.host_header : req.host).append(HTTP_CRLF);
    }
    if (!h.Has("User-Agent")) {
        head.append("User-Agent: coco\r\n");
    }
    bool wants_length = !body.empty() || req.method == "POST" || req.method == "PUT" ||
                        req.method == "PATCH";
    if (wants_length && !h.Has("Content-Length") && !h.Has("Transfer-Encoding")) {
        head.append("Content-Length: ").append(std::to_string(body.size())).append(HTTP_CRLF);
    }
    if (req.close && !h.Has("Connection")) {
        head.append("Connection: close\r\n");
    }
    h.WriteTo(&head);
    head.append(HTTP_CRLF);

    // Go's rule: only these may be sent twice without the caller knowing.
    bool replayable = req.method == "GET" || req.method == "HEAD" || req.method == "OPTIONS" ||
                      req.method == "TRACE";
    for (int attempt = 0;; ++attempt) {
        std::unique_ptr<HttpClientConn> c = pool_->Get(u.key);
        bool reused = c != nullptr;
        int ret;
        if (!reused) {
            std::unique_ptr<StreamConn> conn;
            if ((ret = dial(u.host, u.port, timeout_us, &conn)) != COCO_SUCCESS) {
                coco_warn("http: dial %s:%d failed. ret=%d", u.host.c_str(), u.port, ret);
                return ret;
            }
            c.reset(new HttpClientConn(u.key, std::move(conn)));
        }
        c->conn->SetTimeout(timeout_us);

        iovec iov[2] = {{(void *)head.data(), head.size()}, {(void *)body.data(), body.size()}};
        bool wrote = (ret = c->conn->Writev(iov, body.empty() ? 1 : 2, nullptr)) == COCO_SUCCESS;

        std::unique_ptr<HttpResponse> resp(new HttpResponse());
        while (ret == COCO_SUCCESS) {
            ret = ReadHttpResponse(&c->br, max_header_bytes, req.method, resp.get());
            // 1xx before the final response, e.g. 100 Continue, are skipped.
            if (ret != COCO_SUCCESS || resp->status_code < 100 || resp->status_code > 199 ||
                resp->status_code == 101) {
                break;
            }
        }
        if (ret != COCO_SUCCESS) {
            // A pooled connection may have been closed by the server while idle.
            bool stale = reused && attempt == 0 &&
                         (!wrote || (replayable && (ret == ERROR_SOCKET_READ ||
                                                    ret == ERROR_SOCKET_WRITE)));
            if (stale) {
                continue;
            }
            return ret;
        }

        resp->conn_ = std::move(c);
        resp->pool_ = pool_;
        *out = std::move(resp);
        return COCO_SUCCESS;
    }
}

HttpClient &HttpDefaultClient() {
    // Never destroyed: responses and coroutines may still use it during exit.
    static HttpClient *client = new HttpClient();
    return *client;
}

int HttpGet(const std::string &url, std::unique_ptr<HttpResponse> *resp) {
    return HttpDefaultClient().Get(url, resp);
}

int HttpPost(const std::string &url, const std::string &content_type, const std::string &body,
             std::unique_ptr<HttpResponse> *resp) {
    return HttpDefaultClient().Post(url, content_type, body, resp);
}
