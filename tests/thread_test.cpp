// Several threads, each with a runtime of its own: CocoThread, a connection moved between
// threads, and the servers that spread their connections over worker threads.

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "st.h"

#include "coco/base/coco_thread.hpp"
#include "coco/base/shutdown.hpp"
#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/net/layer4/coco_tcp.hpp"
#include "coco/net/layer7/http/coco_http.hpp"
#include "coco/net/tls/coco_tls.hpp"
#include "coco/server/coco_http_server.hpp"
#include "coco/server/coco_tcp_server.hpp"
#include "test_util.hpp"

using namespace coco;

namespace {

const char *kLoopback = "127.0.0.1";
const int kTimeoutUs = 1000 * 1000;

int Echo(StreamConn &conn) {
    char buf[256];
    ssize_t n = 0;
    int ret;
    while ((ret = conn.Read(buf, sizeof(buf), &n)) == COCO_SUCCESS) {
        if ((ret = conn.Write(buf, n, nullptr)) != COCO_SUCCESS) {
            break;
        }
    }
    return ret;
}

std::unique_ptr<TcpConn> Dial(int port) {
    std::unique_ptr<TcpConn> c;
    cotest::WaitUntil([&]() { return DialTcp(kLoopback, port, kTimeoutUs, &c) == COCO_SUCCESS; });
    if (c) {
        c->SetTimeout(kTimeoutUs);
    }
    return c;
}

bool Echoes(StreamConn *c, const std::string &data) {
    if (c->Write((void *)data.data(), data.size(), nullptr) != COCO_SUCCESS) {
        return false;
    }
    std::string got(data.size(), '\0');
    ssize_t n = 0;
    return c->ReadFully(&got[0], got.size(), &n) == COCO_SUCCESS && got == data;
}

bool PeerClosed(TcpConn *c) {
    char b;
    ssize_t n = 0;
    return c->Read(&b, 1, &n) == ERROR_SOCKET_READ && n == 0;
}

long ElapsedMs(st_utime_t since) { return (long)((st_utime() - since) / 1000); }

struct ShutdownGuard {
    ~ShutdownGuard() { ResetShutdown(); }
};

}  // namespace

// Two threads serve and dial at the same time, each on its own runtime.
COTEST(ThreadsRunSideBySide) {
    std::atomic<int> echoed(0);
    auto serve = [&echoed](int port) {
        TcpServer server(Echo);
        if (server.Start(kLoopback, port) != COCO_SUCCESS) {
            return;
        }
        std::unique_ptr<TcpConn> c = Dial(port);
        for (int i = 0; c && i < 100; ++i) {
            if (Echoes(c.get(), "ping " + std::to_string(i))) {
                ++echoed;
            }
        }
    };
    std::thread a(serve, 19341);
    std::thread b(serve, 19342);
    a.join();
    b.join();
    CHECK_EQ(echoed.load(), 200);
}

// A posted function runs on the worker, on a coroutine of its own; one posted before
// Start() runs once it has started.
COTEST(CocoThreadRunsPostedFunctions) {
    CocoThread worker;
    std::atomic<int> ran(0);
    std::thread::id where;
    int cid = 0;
    CHECK_EQ(worker.Post([&]() {
                 where = std::this_thread::get_id();
                 ++ran;
             }),
             COCO_SUCCESS);
    CHECK_EQ(worker.Load(), 1);
    CHECK_EQ(worker.Start(), COCO_SUCCESS);
    CHECK_EQ(worker.Post([&]() {
                 CocoSleepMs(1);
                 cid = CocoGetCoroutineID();
                 ++ran;
             }),
             COCO_SUCCESS);
    CHECK(cotest::WaitUntil([&]() { return ran == 2 && worker.Load() == 0; }));
    CHECK(where != std::this_thread::get_id());
    CHECK(cid > 0);
    CHECK(cid != CocoGetCoroutineID());
    worker.Stop();
}

// Stop() interrupts what still runs: a blocking call fails and CocoShouldStop() turns true.
COTEST(CocoThreadStopInterruptsTasks) {
    CocoThread worker;
    CHECK_EQ(worker.Start(), COCO_SUCCESS);
    std::atomic<int> started(0), stopped(0);
    for (int i = 0; i < 3; ++i) {
        worker.Post([&]() {
            ++started;
            while (!CocoShouldStop()) {
                CocoSleepMs(10 * 1000);
            }
            ++stopped;
        });
    }
    CHECK(cotest::WaitUntil([&]() { return started == 3; }));
    st_utime_t begin = st_utime();
    worker.Stop();
    CHECK(ElapsedMs(begin) < 1000);
    CHECK_EQ(stopped.load(), 3);
    CHECK_EQ(worker.Load(), 0);
}

// While Stop() waits for a task that takes a while to finish, the other coroutines of the
// caller's thread keep running.
COTEST(CocoThreadStopOnlySuspendsTheCaller) {
    CocoThread worker;
    CHECK_EQ(worker.Start(), COCO_SUCCESS);
    std::atomic<bool> started(false);
    worker.Post([&]() {
        started = true;
        while (!CocoShouldStop()) {
            CocoSleepMs(10 * 1000);
        }
        // Cleaning up after the interrupt; only the first blocking call failed.
        CocoSleepMs(100);
    });
    CHECK(cotest::WaitUntil([&]() { return started.load(); }));

    int ticks = 0;
    bool done = false;
    st_thread_t ticker = cotest::Go([&]() {
        while (!done) {
            ++ticks;
            CocoSleepMs(1);
        }
    });
    worker.Stop();
    done = true;
    st_thread_join(ticker, nullptr);
    CHECK(ticks > 20);
}

COTEST(CocoThreadLifecycleErrors) {
    CocoThread worker;
    CHECK_EQ(worker.Start(), COCO_SUCCESS);
    CHECK_EQ(worker.Start(), ERROR_THREAD_STARTED);
    CHECK_EQ(worker.Post(nullptr), ERROR_SYSTEM_ASSERT_FAILED);
    worker.Stop();
    worker.Stop();
    bool ran = false;
    CHECK_EQ(worker.Post([&]() { ran = true; }), ERROR_THREAD_DISPOSED);
    CHECK(!ran);
    CHECK_EQ(worker.Load(), 0);

    CocoThread never_started;
    never_started.Stop();
    CHECK_EQ(never_started.Start(), ERROR_THREAD_DISPOSED);
}

// Two coroutines stopping one worker: whichever returns, the thread is gone by then.
COTEST(CocoThreadConcurrentStops) {
    CocoThread worker;
    CHECK_EQ(worker.Start(), COCO_SUCCESS);
    std::atomic<bool> finished(false);
    worker.Post([&]() {
        while (!CocoShouldStop()) {
            CocoSleepMs(10 * 1000);
        }
        CocoSleepMs(50);
        finished = true;
    });
    bool returned_early[2] = {true, true};
    st_thread_t a = cotest::Go([&]() {
        worker.Stop();
        returned_early[0] = !finished;
    });
    st_thread_t b = cotest::Go([&]() {
        worker.Stop();
        returned_early[1] = !finished;
    });
    st_thread_join(a, nullptr);
    st_thread_join(b, nullptr);
    CHECK(!returned_early[0]);
    CHECK(!returned_early[1]);
}

// A shutdown interrupts the functions running on a worker, which itself keeps running.
COTEST(CocoThreadShutdownInterruptsTasks) {
    ShutdownGuard guard;
    CocoThread worker;
    CHECK_EQ(worker.Start(), COCO_SUCCESS);
    std::atomic<bool> started(false);
    worker.Post([&]() {
        started = true;
        while (!CocoShouldStop()) {
            CocoSleepMs(10 * 1000);
        }
    });
    CHECK(cotest::WaitUntil([&]() { return started.load(); }));
    CocoShutdown();
    CHECK(cotest::WaitUntil([&]() { return worker.Load() == 0; }));

    std::atomic<bool> later(false);
    CHECK_EQ(worker.Post([&]() { later = true; }), COCO_SUCCESS);
    CHECK(cotest::WaitUntil([&]() { return later.load(); }));
    worker.Stop();
}

// A thread coco knows nothing about may request the shutdown; the waiter here wakes.
COTEST(ShutdownFromPlainThreadWakesWaiter) {
    ShutdownGuard guard;
    int got = -1;
    st_thread_t waiter = cotest::Go([&]() { got = CocoWaitForShutdown(); });
    CocoSleepMs(5);
    CHECK_EQ(got, -1);
    std::thread([]() { CocoShutdown(); }).join();
    st_thread_join(waiter, nullptr);
    CHECK_EQ(got, 0);
}

// A connection accepted here is released, posted to a worker, and served there.
COTEST(TcpConnMovesToAnotherThread) {
    const int port = 19343;
    std::unique_ptr<TcpListener> l;
    CHECK_EQ(ListenTcp(kLoopback, port, &l), COCO_SUCCESS);
    if (!l) {
        return;
    }
    CocoThread worker;
    CHECK_EQ(worker.Start(), COCO_SUCCESS);

    std::unique_ptr<TcpConn> client;
    st_thread_t dialer = cotest::Go([&]() { client = Dial(port); });
    std::unique_ptr<TcpConn> accepted;
    CHECK_EQ(l->AcceptTcp(&accepted), COCO_SUCCESS);
    st_thread_join(dialer, nullptr);
    if (!accepted || !client) {
        CHECK(false);
        return;
    }

    int fd = -1;
    CHECK_EQ(accepted->Release(&fd), COCO_SUCCESS);
    CHECK(fd >= 0);
    char b;
    ssize_t n = 0;
    CHECK_EQ(accepted->Read(&b, 1, &n), ERROR_SOCKET_CLOSED);
    int again = -1;
    CHECK_EQ(accepted->Release(&again), ERROR_SOCKET_CLOSED);
    CHECK(accepted->RemoteAddr().empty());
    accepted.reset();

    std::atomic<bool> served(false);
    worker.Post([fd, &served]() {
        std::unique_ptr<TcpConn> conn;
        if (TcpConnFromFd(fd, &conn) == COCO_SUCCESS) {
            served = true;
            Echo(*conn);
        }
    });
    CHECK(Echoes(client.get(), "moved"));
    CHECK(served.load());
    client.reset();
    CHECK(cotest::WaitUntil([&]() { return worker.Load() == 0; }));
    worker.Stop();
}

COTEST(TcpConnFromBadFdFails) {
    std::unique_ptr<TcpConn> conn;
    CHECK(TcpConnFromFd(-1, &conn) != COCO_SUCCESS);
    CHECK(conn == nullptr);
}

// Connections are spread over the workers, the least loaded first, and Stop() ends them
// all.
COTEST(TcpServerThreadsSpreadConnections) {
    const int port = 19344;
    std::mutex mu;
    std::set<std::thread::id> threads;
    TcpServerOptions opt;
    opt.threads = 4;
    TcpServer server(
        [&](StreamConn &conn) {
            {
                std::lock_guard<std::mutex> lock(mu);
                threads.insert(std::this_thread::get_id());
            }
            return Echo(conn);
        },
        opt);
    CHECK_EQ(server.Start(kLoopback, port), COCO_SUCCESS);

    std::vector<std::unique_ptr<TcpConn>> clients;
    for (int i = 0; i < 16; ++i) {
        std::unique_ptr<TcpConn> c = Dial(port);
        CHECK(c != nullptr && Echoes(c.get(), "c" + std::to_string(i)));
        clients.push_back(std::move(c));
    }
    CHECK_EQ(server.ConnCount(), 16);
    {
        std::lock_guard<std::mutex> lock(mu);
        CHECK_EQ(threads.size(), 4);
        CHECK(threads.count(std::this_thread::get_id()) == 0);
    }

    server.Stop();
    CHECK_EQ(server.ConnCount(), 0);
    for (auto &c : clients) {
        CHECK(c && PeerClosed(c.get()));
    }
    std::unique_ptr<TcpConn> refused;
    CHECK(DialTcp(kLoopback, port, kTimeoutUs, &refused) != COCO_SUCCESS);
}

// The workers handshake TLS themselves.
COTEST(TcpServerThreadsServeTls) {
    const int port = 19345;
    TcpServerOptions opt;
    opt.threads = 2;
    opt.tls_key_file = COCO_SOURCE_DIR "/examples/http-server/server.key";
    opt.tls_crt_file = COCO_SOURCE_DIR "/examples/http-server/server.crt";
    TcpServer server(Echo, opt);
    CHECK_EQ(server.Start(kLoopback, port), COCO_SUCCESS);

    std::shared_ptr<TlsConfig> cfg;
    CHECK_EQ(TlsConfig::NewClient(&cfg), COCO_SUCCESS);
    for (int i = 0; i < 4; ++i) {
        std::unique_ptr<TcpConn> tcp = Dial(port);
        CHECK(tcp != nullptr);
        if (!tcp) {
            return;
        }
        TlsConn tls(std::move(tcp), cfg);
        CHECK_EQ(tls.Handshake(), COCO_SUCCESS);
        CHECK(Echoes(&tls, "over tls " + std::to_string(i)));
    }
}

// A shutdown wakes ListenAndServe on the server's thread, which stops the workers.
COTEST(TcpServerThreadsStopOnShutdown) {
    ShutdownGuard guard;
    const int port = 19346;
    TcpServerOptions opt;
    opt.threads = 2;
    TcpServer server(Echo, opt);
    int ret = -1;
    st_thread_t t = cotest::Go([&]() { ret = server.ListenAndServe(kLoopback, port); });

    std::unique_ptr<TcpConn> a = Dial(port);
    std::unique_ptr<TcpConn> b = Dial(port);
    CHECK(a && Echoes(a.get(), "a"));
    CHECK(b && Echoes(b.get(), "b"));

    CocoShutdown();
    st_thread_join(t, nullptr);
    CHECK_EQ(ret, COCO_SUCCESS);
    CHECK_EQ(server.ConnCount(), 0);
    CHECK(a && PeerClosed(a.get()));
    CHECK(b && PeerClosed(b.get()));
}

// Only a TCP connection's fd can move between threads.
COTEST(TcpServerThreadsNeedTcpListener) {
    std::unique_ptr<TcpListener> l;
    CHECK_EQ(ListenTcp(kLoopback, 19347, &l), COCO_SUCCESS);
    std::shared_ptr<TlsConfig> cfg;
    CHECK_EQ(TlsConfig::NewServer(COCO_SOURCE_DIR "/examples/http-server/server.key",
                                  COCO_SOURCE_DIR "/examples/http-server/server.crt", &cfg),
             COCO_SUCCESS);
    std::unique_ptr<StreamListener> tls(new TlsListener(std::move(l), cfg));
    TcpServerOptions opt;
    opt.threads = 2;
    TcpServer server(Echo, opt);
    CHECK_EQ(server.Start(std::move(tls)), ERROR_SYSTEM_CONFIG_INVALID);
}

COTEST(HttpServerThreads) {
    const int port = 19348;
    HttpServeOptions opt;
    opt.threads = 3;
    HttpServer server(
        [](HttpResponseWriter &w, HttpRequest &r) { w.Write("hello " + r.path); }, opt);
    CHECK_EQ(server.Start(kLoopback, port), COCO_SUCCESS);
    CHECK(Dial(port) != nullptr);

    HttpClient client(kTimeoutUs);
    for (int i = 0; i < 6; ++i) {
        std::unique_ptr<HttpResponse> resp;
        std::string path = "/" + std::to_string(i);
        CHECK_EQ(client.Get("http://127.0.0.1:" + std::to_string(port) + path, &resp),
                 COCO_SUCCESS);
        std::string body;
        if (resp) {
            CHECK_EQ(resp->body.ReadAll(&body), COCO_SUCCESS);
        }
        CHECK(body == "hello " + path);
    }
}
