// HTTP/1.1: the server's framing and connection handling, HttpServeMux routing, and
// HttpClient's pooling, retries and redirects.

#include <stdlib.h>
#include <strings.h>

#include <memory>
#include <set>
#include <string>

#include "coco_api.h"
#include "common/error.hpp"
#include "net/layer4/coco_tcp.hpp"
#include "net/layer7/http/coco_http.hpp"
#include "server/coco_http_server.hpp"
#include "server/coco_tcp_server.hpp"
#include "test_util.hpp"

namespace {

const char *kLoopback = "127.0.0.1";
const int kTimeoutUs = 1000 * 1000;

std::string Url(int port, const std::string &path) {
    return "http://127.0.0.1:" + std::to_string(port) + path;
}

struct RawResponse {
    bool ok = false;
    int status = 0;
    std::string head;
    std::string body;

    std::string Header(const std::string &name) const {
        std::string key = "\r\n" + name + ": ";
        size_t pos = head.find(key);
        if (pos == std::string::npos) {
            return "";
        }
        pos += key.size();
        return head.substr(pos, head.find("\r\n", pos) - pos);
    }
};

// A hand-written client that reads responses one by one, so pipelining is visible.
class RawConn {
 public:
    explicit RawConn(int port) {
        if (DialTcp(kLoopback, port, kTimeoutUs, &c_) == COCO_SUCCESS) {
            c_->SetTimeout(kTimeoutUs);
        }
    }
    bool Send(const std::string &data) {
        return c_ && c_->Write((void *)data.data(), data.size(), nullptr) == COCO_SUCCESS;
    }
    // Reads one response; head_only for responses to HEAD.
    RawResponse Read(bool head_only = false) {
        RawResponse r;
        size_t end;
        while ((end = buf_.find("\r\n\r\n")) == std::string::npos) {
            if (!More()) {
                return r;
            }
        }
        r.head = buf_.substr(0, end + 2);
        buf_.erase(0, end + 4);
        r.status = atoi(r.head.c_str() + 9);
        if (head_only || r.status == 204 || r.status == 304 || r.status < 200) {
            r.ok = true;
            return r;
        }
        if (r.Header("Transfer-Encoding") == "chunked") {
            while (true) {
                size_t lf;
                while ((lf = buf_.find("\r\n")) == std::string::npos) {
                    if (!More()) return r;
                }
                size_t n = strtoul(buf_.c_str(), nullptr, 16);
                buf_.erase(0, lf + 2);
                while (buf_.size() < n + 2) {
                    if (!More()) return r;
                }
                r.body += buf_.substr(0, n);
                buf_.erase(0, n + 2);
                if (n == 0) break;
            }
        } else if (!r.Header("Content-Length").empty()) {
            size_t n = strtoul(r.Header("Content-Length").c_str(), nullptr, 10);
            while (buf_.size() < n) {
                if (!More()) return r;
            }
            r.body = buf_.substr(0, n);
            buf_.erase(0, n);
        } else {
            while (More()) {
            }
            r.body.swap(buf_);
        }
        r.ok = true;
        return r;
    }
    // Whether the server closed the connection, after any unread bytes.
    bool PeerClosed() {
        while (More()) {
        }
        return closed_;
    }
    const std::string &Buffered() const { return buf_; }
    bool More() {
        char tmp[4096];
        ssize_t n = 0;
        int ret = c_ ? c_->Read(tmp, sizeof(tmp), &n) : ERROR_SOCKET_READ;
        if (ret != COCO_SUCCESS) {
            closed_ = ret == ERROR_SOCKET_READ && n == 0;
            return false;
        }
        buf_.append(tmp, n);
        return true;
    }

 private:
    std::unique_ptr<TcpConn> c_;
    std::string buf_;
    bool closed_ = false;
};

std::string Get(const std::string &path, const std::string &extra = "") {
    return "GET " + path + " HTTP/1.1\r\nHost: test\r\n" + extra + "\r\n";
}

std::string ReadBody(HttpResponse *resp) {
    std::string body;
    resp->body.ReadAll(&body);
    return body;
}

}  // namespace

// A small body goes out with a computed Content-Length, sniffed type and Date.
COTEST(HttpSmallResponseHasContentLength) {
    const int port = 19301;
    HttpServer server([](HttpResponseWriter &w, HttpRequest &r) {
        w.Write("hello ");
        w.Write("world");
    });
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    RawConn c(port);
    CHECK(c.Send(Get("/")));
    RawResponse r = c.Read();
    CHECK(r.ok && r.status == 200);
    CHECK(r.Header("Content-Length") == "11");
    CHECK(r.Header("Transfer-Encoding").empty());
    CHECK(r.Header("Content-Type") == "text/plain; charset=utf-8");
    CHECK(!r.Header("Date").empty());
    CHECK(r.body == "hello world");
}

// A body larger than the buffer without Content-Length is chunked; Flush also chunks.
COTEST(HttpLargeResponseIsChunked) {
    const int port = 19302;
    std::string big(20000, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = 'a' + i % 26;
    HttpServeMux mux;
    mux.HandleFunc("/big", [&big](HttpResponseWriter &w, HttpRequest &r) {
        for (size_t i = 0; i < big.size(); i += 1000) w.Write(big.data() + i, 1000);
    });
    mux.HandleFunc("/flush", [](HttpResponseWriter &w, HttpRequest &r) {
        w.Write("a");
        w.Flush();
        w.Write("b");
    });
    mux.HandleFunc("/sized", [&big](HttpResponseWriter &w, HttpRequest &r) {
        w.Header().Set("Content-Length", std::to_string(big.size()));
        w.Write(big);
    });
    HttpServer server(&mux);
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    RawConn c(port);
    CHECK(c.Send(Get("/big") + Get("/flush") + Get("/sized")));
    RawResponse r = c.Read();
    CHECK(r.ok && r.Header("Transfer-Encoding") == "chunked" && r.body == big);
    r = c.Read();
    CHECK(r.ok && r.Header("Transfer-Encoding") == "chunked" && r.body == "ab");
    r = c.Read();
    CHECK(r.ok && r.Header("Content-Length") == std::to_string(big.size()) && r.body == big);
}

// Pipelined requests arrive in one segment and are answered in order.
COTEST(HttpPipelinedRequests) {
    const int port = 19303;
    HttpServer server([](HttpResponseWriter &w, HttpRequest &r) { w.Write(r.path); });
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    RawConn c(port);
    CHECK(c.Send(Get("/a") + Get("/b") + Get("/c", "Connection: close\r\n")));
    CHECK(c.Read().body == "/a");
    CHECK(c.Read().body == "/b");
    RawResponse r = c.Read();
    CHECK(r.body == "/c" && r.Header("Connection") == "close");
    CHECK(c.PeerClosed());
}

// Bodies by Content-Length and chunked are read; one the handler skipped is drained so
// the next request on the connection still parses.
COTEST(HttpRequestBodies) {
    const int port = 19304;
    HttpServeMux mux;
    mux.HandleFunc("POST /echo", [](HttpResponseWriter &w, HttpRequest &r) {
        std::string body;
        CHECK_EQ(r.body.ReadAll(&body), COCO_SUCCESS);
        w.Write(body);
    });
    mux.HandleFunc("POST /ignore", [](HttpResponseWriter &w, HttpRequest &r) { w.Write("ok"); });
    HttpServer server(&mux);
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    RawConn c(port);
    CHECK(c.Send("POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\n\r\nhello"));
    CHECK(c.Read().body == "hello");
    CHECK(c.Send("POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "3;ext=1\r\nabc\r\n4\r\ndefg\r\n0\r\nX-Trailer: 1\r\n\r\n"));
    CHECK(c.Read().body == "abcdefg");
    CHECK(c.Send("POST /ignore HTTP/1.1\r\nHost: t\r\nContent-Length: 6\r\n\r\nunread" + Get("/x")));
    CHECK(c.Read().body == "ok");
    CHECK_EQ(c.Read().status, 404);
}

// "100 Continue" is sent only once the handler reads the body.
COTEST(HttpExpectContinue) {
    const int port = 19305;
    HttpServer server([](HttpResponseWriter &w, HttpRequest &r) {
        std::string body;
        r.body.ReadAll(&body);
        w.Write(body);
    });
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    RawConn c(port);
    CHECK(c.Send("POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n"));
    RawResponse r = c.Read();
    CHECK_EQ(r.status, 100);
    CHECK(c.Send("data"));
    r = c.Read();
    CHECK(r.status == 200 && r.body == "data");
}

// HEAD gets the GET handler's headers without the body, and keep-alive goes on.
COTEST(HttpHeadRequest) {
    const int port = 19306;
    HttpServeMux mux;
    mux.HandleFunc("GET /doc", [](HttpResponseWriter &w, HttpRequest &r) { w.Write("content"); });
    HttpServer server(&mux);
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    RawConn c(port);
    CHECK(c.Send("HEAD /doc HTTP/1.1\r\nHost: t\r\n\r\n" + Get("/doc")));
    RawResponse r = c.Read(true);
    CHECK(r.status == 200 && r.Header("Content-Length") == "7");
    r = c.Read();
    CHECK(r.status == 200 && r.body == "content");
}

// Malformed requests are answered without reaching a handler, then the connection closes.
COTEST(HttpBadRequests) {
    const int port = 19307;
    int calls = 0;
    HttpServeOptions opt;
    opt.max_header_bytes = 1024;
    HttpServer server([&calls](HttpResponseWriter &w, HttpRequest &r) { ++calls; }, opt);
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    struct Case {
        std::string request;
        int status;
    } cases[] = {
        {"NOT HTTP\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: t\r\nX: " + std::string(2000, 'a') + "\r\n\r\n", 431},
        {"GET / HTTP/1.1\r\nHost: t\r\nExpect: magic\r\n\r\n", 417},
    };
    for (const Case &tc : cases) {
        RawConn c(port);
        CHECK(c.Send(tc.request));
        CHECK_EQ(c.Read().status, tc.status);
        CHECK(c.PeerClosed());
    }
    CHECK_EQ(calls, 0);
}

// HTTP/1.0 closes unless asked to keep alive, and a response without length then ends
// with the connection.
COTEST(HttpOneZero) {
    const int port = 19308;
    HttpServer server([](HttpResponseWriter &w, HttpRequest &r) { w.Write("x"); });
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);
    {
        RawConn c(port);
        CHECK(c.Send("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"));
        RawResponse r = c.Read();
        CHECK(r.body == "x" && r.Header("Connection") == "keep-alive");
        CHECK(c.Send("GET / HTTP/1.0\r\n\r\n"));
        r = c.Read();
        CHECK(r.body == "x" && r.Header("Connection") == "close");
        CHECK(c.PeerClosed());
    }
}

// Routing: specificity, wildcards, methods, hosts and redirects.
COTEST(HttpMuxRouting) {
    const int port = 19309;
    HttpServeMux mux;
    auto reply = [](const std::string &name) {
        return [name](HttpResponseWriter &w, HttpRequest &r) {
            std::string s = name;
            if (!r.PathValue("id").empty()) s += " id=" + r.PathValue("id");
            if (!r.PathValue("rest").empty()) s += " rest=" + r.PathValue("rest");
            w.Write(s);
        };
    };
    CHECK_EQ(mux.HandleFunc("/", reply("root")), COCO_SUCCESS);
    CHECK_EQ(mux.HandleFunc("/{$}", reply("home")), COCO_SUCCESS);
    CHECK_EQ(mux.HandleFunc("/static/", reply("static")), COCO_SUCCESS);
    CHECK_EQ(mux.HandleFunc("/static/img/", reply("img")), COCO_SUCCESS);
    CHECK_EQ(mux.HandleFunc("GET /users/{id}", reply("user")), COCO_SUCCESS);
    CHECK_EQ(mux.HandleFunc("DELETE /users/{id}", reply("deluser")), COCO_SUCCESS);
    CHECK_EQ(mux.HandleFunc("GET /users/me", reply("me")), COCO_SUCCESS);
    CHECK_EQ(mux.HandleFunc("/files/{rest...}", reply("files")), COCO_SUCCESS);
    CHECK_EQ(mux.HandleFunc("POST /only-post", reply("post")), COCO_SUCCESS);
    CHECK_EQ(mux.HandleFunc("example.com/", reply("example")), COCO_SUCCESS);
    CHECK_EQ(mux.HandleFunc("GET /users/me", reply("dup")), ERROR_HTTP_PATTERN_DUPLICATED);
    CHECK(mux.HandleFunc("no-slash", reply("x")) != COCO_SUCCESS);
    CHECK(mux.HandleFunc("/a/{x...}/b", reply("x")) != COCO_SUCCESS);
    HttpServer server(&mux);
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    struct Case {
        std::string request;
        int status;
        std::string body;
        std::string location;
    } cases[] = {
        {Get("/"), 200, "home", ""},
        {Get("/anything"), 200, "root", ""},
        {Get("/static/a.css"), 200, "static", ""},
        {Get("/static/img/a.png"), 200, "img", ""},
        {Get("/static"), 301, "", "/static/"},
        {Get("/users/42"), 200, "user id=42", ""},
        {Get("/users/me"), 200, "me", ""},
        {Get("/users/a%20b"), 200, "user id=a b", ""},
        {"DELETE /users/7 HTTP/1.1\r\nHost: t\r\n\r\n", 200, "deluser id=7", ""},
        // A pattern without method, here "/", matches any method, as in Go.
        {"PUT /users/7 HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n", 200, "root", ""},
        {Get("/files/a/b.txt"), 200, "files rest=a/b.txt", ""},
        {Get("/files/"), 200, "files", ""},
        {Get("/only-post"), 200, "root", ""},
        {Get("/a/../static/x?q=1"), 301, "", "/static/x?q=1"},
        {Get("//users/1"), 301, "", "/users/1"},
        {"GET / HTTP/1.1\r\nHost: example.com:8080\r\n\r\n", 200, "example", ""},
    };
    RawConn c(port);
    for (const Case &tc : cases) {
        CHECK(c.Send(tc.request));
        RawResponse r = c.Read();
        if (r.status != tc.status || (!tc.body.empty() && r.body != tc.body) ||
            r.Header("Location") != tc.location) {
            fprintf(stderr, "request %s-> %d [%s] location=%s\n", tc.request.c_str(), r.status,
                    r.body.c_str(), r.Header("Location").c_str());
        }
        CHECK_EQ(r.status, tc.status);
        CHECK(tc.body.empty() || r.body == tc.body);
        CHECK(r.Header("Location") == tc.location);
    }
}

// Without a pattern for every method, a path that matched only by method gets 405.
COTEST(HttpMuxMethodNotAllowed) {
    const int port = 19315;
    HttpServeMux mux;
    auto ok = [](HttpResponseWriter &w, HttpRequest &r) { w.Write(r.method); };
    mux.HandleFunc("GET /users/{id}", ok);
    mux.HandleFunc("DELETE /users/{id}", ok);
    HttpServer server(&mux);
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    RawConn c(port);
    CHECK(c.Send("PUT /users/7 HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n"));
    RawResponse r = c.Read();
    CHECK_EQ(r.status, 405);
    CHECK(r.Header("Allow") == "DELETE, GET, HEAD");
    CHECK(c.Send(Get("/users/7")));
    CHECK(c.Read().body == "GET");
    CHECK(c.Send(Get("/nope")));
    CHECK_EQ(c.Read().status, 404);
}

// Query values are unescaped; headers compare case-insensitively.
COTEST(HttpRequestFields) {
    const int port = 19310;
    std::string seen;
    HttpServer server([&seen](HttpResponseWriter &w, HttpRequest &r) {
        seen = r.method + "|" + r.path + "|" + r.raw_query + "|" + r.Query().Get("b") + "|" +
               r.Query().Get("c") + "|" + r.header.Get("x-custom") + "|" + r.host;
        CHECK(!r.remote_addr.empty());
    });
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    RawConn c(port);
    CHECK(c.Send(Get("/p%41th?a=1&b=x%20y&c=1+2", "X-Custom: v\r\n")));
    CHECK_EQ(c.Read().status, 200);
    CHECK(seen == "GET|/pAth|a=1&b=x%20y&c=1+2|x y|1 2|v|test");
}

// The client reuses one keep-alive connection for sequential requests.
COTEST(HttpClientReusesConnection) {
    const int port = 19311;
    std::set<std::string> peers;
    HttpServer server([&peers](HttpResponseWriter &w, HttpRequest &r) {
        peers.insert(r.remote_addr);
        std::string body;
        r.body.ReadAll(&body);
        w.Header().Set("X-Method", r.method);
        w.Write(body.empty() ? r.path : body);
    });
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    HttpClient client(kTimeoutUs);
    for (int i = 0; i < 5; ++i) {
        std::unique_ptr<HttpResponse> resp;
        CHECK_EQ(client.Get(Url(port, "/n" + std::to_string(i)), &resp), COCO_SUCCESS);
        CHECK(resp && resp->status_code == 200 && ReadBody(resp.get()) == "/n" + std::to_string(i));
    }
    std::unique_ptr<HttpResponse> resp;
    CHECK_EQ(client.Post(Url(port, "/p"), "text/plain", "payload", &resp), COCO_SUCCESS);
    CHECK(resp->header.Get("X-Method") == "POST" && ReadBody(resp.get()) == "payload");
    // Not even reading the body keeps the connection when the body was already buffered.
    CHECK_EQ(client.Get(Url(port, "/unread"), &resp), COCO_SUCCESS);
    CHECK_EQ(client.Get(Url(port, "/last"), &resp), COCO_SUCCESS);
    CHECK(ReadBody(resp.get()) == "/last");
    CHECK_EQ(peers.size(), 1);

    CHECK_EQ(HttpGet(Url(port, "/default"), &resp), COCO_SUCCESS);
    CHECK(ReadBody(resp.get()) == "/default");
}

// A pooled connection the server closed while idle is replaced transparently.
COTEST(HttpClientRetriesStaleConnection) {
    const int port = 19312;
    int conns = 0;
    // Answers one request per connection with keep-alive, then hangs up anyway.
    TcpServer server([&conns](StreamConn &conn) {
        ++conns;
        BufReader br(&conn);
        HttpRequest req;
        if (ReadHttpRequest(&br, 4096, &req) != COCO_SUCCESS) return 0;
        std::string rsp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        conn.Write((void *)rsp.data(), rsp.size(), nullptr);
        CocoSleepMs(10);
        return 0;
    });
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    HttpClient client(kTimeoutUs);
    for (int i = 0; i < 3; ++i) {
        std::unique_ptr<HttpResponse> resp;
        CHECK_EQ(client.Get(Url(port, "/"), &resp), COCO_SUCCESS);
        CHECK(resp && ReadBody(resp.get()) == "ok");
        resp.reset();
        CocoSleepMs(30);
    }
    CHECK_EQ(conns, 3);
}

// Redirects are followed; 303 turns a POST into a GET.
COTEST(HttpClientFollowsRedirects) {
    const int port = 19313;
    HttpServeMux mux;
    mux.HandleFunc("/old", [](HttpResponseWriter &w, HttpRequest &r) {
        HttpRedirect(w, r, "new?x=1", 302);
    });
    mux.HandleFunc("/submit", [](HttpResponseWriter &w, HttpRequest &r) {
        HttpRedirect(w, r, "/done", 303);
    });
    mux.HandleFunc("/loop", [](HttpResponseWriter &w, HttpRequest &r) {
        HttpRedirect(w, r, "/loop", 307);
    });
    mux.HandleFunc("/", [](HttpResponseWriter &w, HttpRequest &r) {
        w.Write(r.method + " " + r.url);
    });
    HttpServer server(&mux);
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    HttpClient client(kTimeoutUs);
    std::unique_ptr<HttpResponse> resp;
    CHECK_EQ(client.Get(Url(port, "/old"), &resp), COCO_SUCCESS);
    CHECK(ReadBody(resp.get()) == "GET /new?x=1");
    CHECK_EQ(client.Post(Url(port, "/submit"), "text/plain", "data", &resp), COCO_SUCCESS);
    CHECK(ReadBody(resp.get()) == "GET /done");
    CHECK_EQ(client.Get(Url(port, "/loop"), &resp), ERROR_HTTP_TOO_MANY_REDIRECTS);

    client.max_redirects = 0;
    CHECK_EQ(client.Get(Url(port, "/old"), &resp), COCO_SUCCESS);
    CHECK_EQ(resp->status_code, 302);
    CHECK(resp->header.Get("Location") == "/new?x=1");
}

// Chunked responses and responses delimited by the server closing are read to the end.
COTEST(HttpClientResponseFraming) {
    const int port = 19314;
    TcpServer server([](StreamConn &conn) {
        BufReader br(&conn);
        HttpRequest req;
        if (ReadHttpRequest(&br, 4096, &req) != COCO_SUCCESS) return 0;
        std::string rsp = req.path == "/chunked"
                              ? "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"
                              : "HTTP/1.1 200 OK\r\n\r\nuntil close";
        conn.Write((void *)rsp.data(), rsp.size(), nullptr);
        return 0;
    });
    CHECK_EQ(server.ListenAndServe(kLoopback, port), COCO_SUCCESS);

    HttpClient client(kTimeoutUs);
    std::unique_ptr<HttpResponse> resp;
    CHECK_EQ(client.Get(Url(port, "/chunked"), &resp), COCO_SUCCESS);
    CHECK_EQ(resp->content_length, -1);
    CHECK(ReadBody(resp.get()) == "hello world");
    CHECK_EQ(client.Get(Url(port, "/close"), &resp), COCO_SUCCESS);
    CHECK(resp->close);
    CHECK(ReadBody(resp.get()) == "until close");
}

COTEST(HttpClientRejectsBadUrls) {
    HttpClient client(kTimeoutUs);
    std::unique_ptr<HttpResponse> resp;
    CHECK_EQ(client.Get("ftp://127.0.0.1/", &resp), ERROR_HTTP_PARSE_URI);
    CHECK_EQ(client.Get("not a url", &resp), ERROR_HTTP_PARSE_URI);
    CHECK_EQ(client.Get("https://127.0.0.1:1/", &resp), ERROR_HTTPS_NOT_SUPPORTED);
}

COTEST(HttpHelpers) {
    CHECK(HttpCleanPath("") == "/");
    CHECK(HttpCleanPath("/a/b/../c/./d/") == "/a/c/d/");
    CHECK(HttpCleanPath("/../a//b") == "/a/b");
    CHECK(HttpCleanPath("a/b") == "/a/b");

    HttpValues v = HttpValues::Parse("a=1&b=x+y&a=2&bad=%zz&c");
    CHECK(v.Get("a") == "1" && v.Values("a").size() == 2);
    CHECK(v.Get("b") == "x y" && v.Has("c") && !v.Has("bad"));
    CHECK(v.Encode() == "a=1&b=x+y&a=2&c=");

    HttpHeader h;
    h.Add("Connection", "keep-alive, Upgrade");
    h.Set("content-type", "a");
    h.Set("Content-Type", "b");
    CHECK(h.HasToken("connection", "upgrade") && !h.HasToken("Connection", "close"));
    CHECK(h.Get("CONTENT-TYPE") == "b" && h.Size() == 2);
    h.Set("X-Evil", "a\r\nInjected: 1");
    std::string out;
    h.WriteTo(&out);
    CHECK(out.find("\r\nInjected") == std::string::npos);

    CHECK(HttpDetectContentType("<!DOCTYPE html><p>", 18) == "text/html; charset=utf-8");
    CHECK(HttpDetectContentType("\x89PNG\r\n\x1a\nxxxx", 12) == "image/png");
    CHECK(HttpDetectContentType("{\"a\":1}", 7) == "text/plain; charset=utf-8");
    CHECK(HttpDetectContentType("\x00\x01\x02", 3) == "application/octet-stream");
}
