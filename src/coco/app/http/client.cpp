#include "coco/app/http/client.hpp"

#include <string.h>
#include <strings.h>
#include <sys/uio.h>

#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/app/http/codec/url.hpp"
#include "coco/log/log.hpp"
#include "coco/net/tcp.hpp"

namespace coco {

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
    if (!conn_ || close || status_code == HttpStatusSwitchingProtocols ||
        !body.DiscardBuffered()) {
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

HttpClient::HttpClient(int64_t timeout_us)
    : timeout_us(timeout_us), dialer_(TcpDialer()), pool_(std::make_shared<HttpConnPool>()) {}

HttpClient::~HttpClient() { pool_->Clear(); }

int HttpClient::Get(const std::string &url, std::unique_ptr<HttpResponse> *resp) {
    HttpRequest req(HttpMethodGet, url);
    return Do(req, resp);
}

int HttpClient::Post(const std::string &url, const std::string &content_type,
                     const std::string &body, std::unique_ptr<HttpResponse> *resp) {
    HttpRequest req(HttpMethodPost, url, body);
    req.header.Set(HttpHeaderContentType, content_type);
    return Do(req, resp);
}

int HttpClient::Do(HttpRequest &req, std::unique_ptr<HttpResponse> *resp) {
    resp->reset();
    HttpUrl u;
    int ret = ParseHttpUrl(req.url, &u);
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
        const std::string &loc = (*resp)->header.Get(HttpHeaderLocation);
        bool redirect = code == HttpStatusMovedPermanently || code == HttpStatusFound ||
                        code == HttpStatusSeeOther || code == HttpStatusTemporaryRedirect ||
                        code == HttpStatusPermanentRedirect;
        if (!redirect || max_redirects <= 0 || loc.empty()) {
            return COCO_SUCCESS;
        }
        if (redirects >= max_redirects) {
            coco_warn("http: stopped after %d redirects", redirects);
            return ERROR_HTTP_TOO_MANY_REDIRECTS;
        }

        std::string target = HttpResolveLocation(u, loc);
        HttpUrl next;
        if (ParseHttpUrl(target, &next) != COCO_SUCCESS) {
            return COCO_SUCCESS;
        }
        std::unique_ptr<HttpRequest> r(new HttpRequest(cur->method, target, cur->SendBody()));
        r->header = cur->header;
        r->close = cur->close;
        // Like browsers and Go: 301/302/303 turn into a GET without a body.
        if (code <= HttpStatusSeeOther && cur->method != HttpMethodGet &&
            cur->method != HttpMethodHead) {
            r->method = HttpMethodGet;
            r->SetBody("");
            r->header.Del(HttpHeaderContentType);
            r->header.Del(HttpHeaderContentLength);
        }
        if (next.host != u.host) {
            r->header.Del(HttpHeaderAuthorization);
            r->header.Del(HttpHeaderCookie);
            r->header.Del(HttpHeaderWWWAuthenticate);
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
    if (!h.Has(HttpHeaderHost)) {
        head.append("Host: ").append(req.host.empty() ? u.host_header : req.host).append(HTTP_CRLF);
    }
    if (!h.Has(HttpHeaderUserAgent)) {
        head.append("User-Agent: coco\r\n");
    }
    bool wants_length = !body.empty() || req.method == HttpMethodPost ||
                        req.method == HttpMethodPut || req.method == HttpMethodPatch;
    if (wants_length && !h.Has(HttpHeaderContentLength) && !h.Has(HttpHeaderTransferEncoding)) {
        head.append("Content-Length: ").append(std::to_string(body.size())).append(HTTP_CRLF);
    }
    if (req.close && !h.Has(HttpHeaderConnection)) {
        head.append("Connection: close\r\n");
    }
    h.WriteTo(&head);
    head.append(HTTP_CRLF);

    // Go's rule: only these may be sent twice without the caller knowing.
    bool replayable = req.method == HttpMethodGet || req.method == HttpMethodHead ||
                      req.method == HttpMethodOptions || req.method == HttpMethodTrace;
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
                resp->status_code == HttpStatusSwitchingProtocols) {
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
    // One per thread, as its pooled connections belong to that thread's runtime. Never
    // destroyed: responses and coroutines may still use it during exit.
    static thread_local HttpClient *client = nullptr;
    if (client == nullptr) {
        client = new HttpClient();
    }
    return *client;
}

int HttpGet(const std::string &url, std::unique_ptr<HttpResponse> *resp) {
    return HttpDefaultClient().Get(url, resp);
}

int HttpPost(const std::string &url, const std::string &content_type, const std::string &body,
             std::unique_ptr<HttpResponse> *resp) {
    return HttpDefaultClient().Post(url, content_type, body, resp);
}

}  // namespace coco
