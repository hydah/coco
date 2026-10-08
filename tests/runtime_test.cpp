// The runtime around the servers: CocoInit(), shutdown and signals, the blocking
// ListenAndServe that a program's main returns from, and CocoRun().

#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <memory>
#include <string>
#include <thread>

#include "st.h"

#include "coco/base/coco_thread.hpp"
#include "coco/base/shutdown.hpp"
#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/net/tcp.hpp"
#include "coco/app/http/client.hpp"
#include "coco/app/http/server.hpp"
#include "coco/net/tcp_server.hpp"
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

// Dials until the server accepts, since it starts listening on another coroutine.
std::unique_ptr<TcpConn> Dial(int port) {
    std::unique_ptr<TcpConn> c;
    cotest::WaitUntil([&]() { return DialTcp(kLoopback, port, kTimeoutUs, &c) == COCO_SUCCESS; });
    if (c) {
        c->SetTimeout(kTimeoutUs);
    }
    return c;
}

bool Echoes(TcpConn *c, const std::string &data) {
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

// Forgets the shutdown a case asked for, however the case ends.
struct ShutdownGuard {
    ~ShutdownGuard() { ResetShutdown(); }
};

long ElapsedMs(st_utime_t since) { return (long)((st_utime() - since) / 1000); }

std::string SelfPath() {
    char buf[4096];
#ifdef __APPLE__
    uint32_t size = sizeof(buf);
    return _NSGetExecutablePath(buf, &size) == 0 ? buf : "";
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return "";
    }
    buf[n] = '\0';
    return buf;
#endif
}

// Runs this program again with a helper case, its stdout on *out_fd. Real signals need a
// real process: a fresh one, because the event system does not survive a fork.
pid_t SpawnHelper(const char *helper, int *out_fd) {
    std::string self = SelfPath();
    int fds[2];
    if (self.empty() || pipe(fds) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        execl(self.c_str(), self.c_str(), helper, (char *)nullptr);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return -1;
    }
    *out_fd = fds[0];
    return pid;
}

// Blocks the whole process, which is fine here: nothing else has to run meanwhile.
bool WaitForReady(int fd) {
    char buf[16];
    return read(fd, buf, sizeof(buf)) > 0;
}

// False when the child still runs after timeout_ms; it is killed then.
bool WaitForExit(pid_t pid, int timeout_ms, int *status) {
    for (int waited = 0; waited <= timeout_ms; waited += 10) {
        if (waitpid(pid, status, WNOHANG) == pid) {
            return true;
        }
        usleep(10 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, status, 0);
    return false;
}

}  // namespace

COTEST(RuntimeInitIsIdempotent) {
    CHECK_EQ(CocoInit(), COCO_SUCCESS);
    CHECK_EQ(CocoInit(), COCO_SUCCESS);
}

// Another thread sets up a runtime of its own on first use, by itself or through
// CocoInit(), and its coroutine IDs do not repeat this thread's.
COTEST(RuntimeOnEveryThread) {
    CHECK_EQ(CocoInit(), COCO_SUCCESS);
    int main_id = CocoGetCoroutineID();
    int id_before = -1, init = -1, run = -1, id = -1;
    std::thread other([&]() {
        id_before = CocoGetCoroutineID();
        CocoSleepMs(1);
        init = CocoInit();
        run = CocoRun([]() { return 9; });
        id = CocoGetCoroutineID();
    });
    other.join();

    CHECK_EQ(id_before, 0);
    CHECK_EQ(init, COCO_SUCCESS);
    CHECK_EQ(run, 9);
    CHECK(id > 0);
    CHECK(id != main_id);
    CHECK_EQ(CocoGetCoroutineID(), main_id);
}

COTEST(RuntimeShutdownWakesWaiters) {
    int got[2] = {-1, -1};
    st_thread_t a = cotest::Go([&]() { got[0] = CocoWaitForShutdown(); });
    st_thread_t b = cotest::Go([&]() { got[1] = CocoWaitForShutdown(); });
    CocoSleepMs(5);
    CHECK_EQ(got[0], -1);
    CHECK(!CocoShutdownRequested());

    CocoShutdown();
    st_thread_join(a, nullptr);
    st_thread_join(b, nullptr);
    CHECK_EQ(got[0], 0);
    CHECK_EQ(got[1], 0);
    CHECK(CocoShutdownRequested());
    ResetShutdown();
}

// The signal arrives on this thread while a coroutine waits; the waiter learns which.
COTEST(RuntimeSignalWakesWaiter) {
    int got = -1;
    st_thread_t t = cotest::Go([&]() { got = CocoWaitForShutdown(); });
    CocoSleepMs(5);
    CHECK_EQ(got, -1);

    raise(SIGTERM);
    st_thread_join(t, nullptr);
    CHECK_EQ(got, SIGTERM);
    ResetShutdown();
}

COTEST(TcpServerListenAndServeBlocksUntilStop) {
    const int port = 19261;
    TcpServer server(Echo);
    int ret = -1;
    bool returned = false;
    st_thread_t t = cotest::Go([&]() {
        ret = server.ListenAndServe(kLoopback, port);
        returned = true;
    });

    std::unique_ptr<TcpConn> c = Dial(port);
    CHECK(c != nullptr);
    if (c) {
        CHECK(Echoes(c.get(), "blocking"));
    }
    CHECK(!returned);

    server.Stop();
    st_thread_join(t, nullptr);
    CHECK(returned);
    CHECK_EQ(ret, COCO_SUCCESS);
    if (c) {
        CHECK(PeerClosed(c.get()));
    }
}

// A shutdown stops the server before ListenAndServe returns, so no connection is left.
COTEST(TcpServerListenAndServeStopsOnShutdown) {
    const int port = 19262;
    TcpServer server(Echo);
    int ret = -1;
    st_thread_t t = cotest::Go([&]() { ret = server.ListenAndServe(kLoopback, port); });

    std::unique_ptr<TcpConn> c = Dial(port);
    CHECK(c != nullptr);
    CHECK(cotest::WaitUntil([&]() { return server.ConnCount() == 1; }));

    CocoShutdown();
    st_thread_join(t, nullptr);
    CHECK_EQ(ret, COCO_SUCCESS);
    CHECK_EQ(server.ConnCount(), 0);
    if (c) {
        CHECK(PeerClosed(c.get()));
    }
    ResetShutdown();
}

// A failure to listen is returned at once instead of blocking.
COTEST(TcpServerListenAndServeReturnsListenError) {
    const int port = 19263;
    TcpServer first(Echo);
    CHECK_EQ(first.Start(kLoopback, port), COCO_SUCCESS);
    TcpServer second(Echo);
    CHECK(second.ListenAndServe(kLoopback, port) != COCO_SUCCESS);
}

COTEST(TcpServerStartAfterStopFails) {
    TcpServer served(Echo);
    CHECK_EQ(served.Start(kLoopback, 19264), COCO_SUCCESS);
    served.Stop();
    CHECK(served.Start(kLoopback, 19264) != COCO_SUCCESS);

    TcpServer never_served(Echo);
    never_served.Stop();
    CHECK_EQ(never_served.Start(kLoopback, 19265), ERROR_THREAD_DISPOSED);
}

// A handler may end the program: it calls CocoShutdown() and the blocking call returns.
COTEST(HttpServerListenAndServeStopsOnShutdownFromHandler) {
    const int port = 19266;
    HttpServer server([](HttpResponseWriter &w, HttpRequest &) {
        w.Write("bye");
        CocoShutdown();
    });
    int ret = -1;
    st_thread_t t = cotest::Go([&]() { ret = server.ListenAndServe(kLoopback, port); });
    CHECK(Dial(port) != nullptr);

    HttpClient client(kTimeoutUs);
    std::unique_ptr<HttpResponse> resp;
    CHECK_EQ(client.Get("http://127.0.0.1:" + std::to_string(port) + "/quit", &resp),
             COCO_SUCCESS);
    std::string body;
    if (resp) {
        CHECK_EQ(resp->body.ReadAll(&body), COCO_SUCCESS);
    }
    CHECK(body == "bye");
    resp.reset();

    st_thread_join(t, nullptr);
    CHECK_EQ(ret, COCO_SUCCESS);
    ResetShutdown();
}

COTEST(CocoRunReturnsWhatFnReturns) {
    CHECK_EQ(CocoRun([]() { return 42; }), 42);
    CHECK_EQ(CocoRun([]() { return COCO_SUCCESS; }), COCO_SUCCESS);
    CHECK_EQ(CocoRun(nullptr), ERROR_SYSTEM_ASSERT_FAILED);
}

COTEST(CocoRunRejectsNesting) {
    int inner = COCO_SUCCESS;
    CHECK_EQ(CocoRun([&inner]() {
                 inner = CocoRun([]() { return 1; });
                 return 5;
             }),
             5);
    CHECK_EQ(inner, ERROR_THREAD_STARTED);
    // Once it is over another one may start.
    CHECK_EQ(CocoRun([]() { return 6; }), 6);
}

// Only the coroutine of CocoRun()'s function is told to stop, and only while it runs.
COTEST(CocoShouldStopIsFalseOutsideCocoRun) {
    ShutdownGuard guard;
    CHECK(!CocoShouldStop());
    CocoShutdown();
    CHECK(!CocoShouldStop());
}

// The shutdown cuts a sleep short, and the loop that checks CocoShouldStop() ends.
COTEST(CocoRunShutdownEndsSleepLoop) {
    ShutdownGuard guard;
    st_thread_t t = cotest::Go([]() {
        CocoSleepMs(20);
        CocoShutdown();
    });
    int loops = 0;
    bool stop_seen = false;
    st_utime_t begin = st_utime();
    int ret = CocoRun([&]() {
        while (!CocoShouldStop()) {
            ++loops;
            CocoSleepMs(2000);
        }
        stop_seen = CocoShouldStop();
        return 7;
    });
    long took = ElapsedMs(begin);
    st_thread_join(t, nullptr);

    CHECK_EQ(ret, 7);
    CHECK_EQ(loops, 1);
    CHECK(stop_seen);
    CHECK(took < 1000);
    CHECK(CocoShutdownRequested());
}

// A real signal, through the handler, the pipe and the watcher coroutine, ends a read that
// would otherwise block for ever.
COTEST(CocoRunSignalEndsBlockingRead) {
    ShutdownGuard guard;
    const int port = 19271;
    TcpServer server([](StreamConn &conn) {
        char b;
        ssize_t n = 0;
        return conn.Read(&b, 1, &n);
    });
    CHECK_EQ(server.Start(kLoopback, port), COCO_SUCCESS);

    st_thread_t t = cotest::Go([]() {
        CocoSleepMs(20);
        raise(SIGTERM);
    });
    int read_ret = COCO_SUCCESS;
    bool stop_seen = false;
    st_utime_t begin = st_utime();
    int ret = CocoRun([&]() {
        std::unique_ptr<TcpConn> c;
        if (DialTcp(kLoopback, port, kTimeoutUs, &c) != COCO_SUCCESS) {
            return 1;
        }
        char b;
        ssize_t n = 0;
        read_ret = c->Read(&b, 1, &n);
        stop_seen = CocoShouldStop();
        return 0;
    });
    long took = ElapsedMs(begin);
    st_thread_join(t, nullptr);

    CHECK_EQ(ret, 0);
    CHECK(read_ret != COCO_SUCCESS);
    CHECK(stop_seen);
    CHECK(took < 1000);
}

// ListenAndServe on the main coroutine, under CocoRun: the shutdown both wakes the wait and
// interrupts the coroutine, and the server still stops cleanly before it returns.
COTEST(CocoRunStopsBlockingListenAndServe) {
    ShutdownGuard guard;
    const int port = 19272;
    TcpServer server(Echo);
    std::unique_ptr<TcpConn> client;
    st_thread_t t = cotest::Go([&]() {
        client = Dial(port);
        CocoSleepMs(20);
        CocoShutdown();
    });
    int ret = CocoRun([&]() { return server.ListenAndServe(kLoopback, port); });
    st_thread_join(t, nullptr);

    CHECK_EQ(ret, COCO_SUCCESS);
    CHECK_EQ(server.ConnCount(), 0);
    CHECK(client != nullptr);
    if (client) {
        CHECK(PeerClosed(client.get()));
    }
    std::unique_ptr<TcpConn> refused;
    CHECK(DialTcp(kLoopback, port, kTimeoutUs, &refused) != COCO_SUCCESS);
}

// Asked to stop before it started: the first blocking call fails at once, and no interrupt
// is left pending for whatever comes after CocoRun().
COTEST(CocoRunAfterShutdownStopsAtOnce) {
    ShutdownGuard guard;
    CocoShutdown();
    bool stop_seen = false;
    int slept = 0;
    st_utime_t begin = st_utime();
    int ret = CocoRun([&]() {
        stop_seen = CocoShouldStop();
        slept = st_usleep(2000 * 1000);
        return 3;
    });
    CHECK_EQ(ret, 3);
    CHECK(stop_seen);
    CHECK_EQ(slept, -1);
    CHECK(ElapsedMs(begin) < 500);

    ResetShutdown();
    begin = st_utime();
    CocoSleepMs(30);
    CHECK(ElapsedMs(begin) >= 25);
}

// Stop() closes the listening socket, so new clients are refused instead of left hanging in
// the backlog until the server is destroyed.
COTEST(TcpServerStopClosesListener) {
    const int port = 19274;
    TcpServer server(Echo);
    CHECK_EQ(server.Start(kLoopback, port), COCO_SUCCESS);
    CHECK(Dial(port) != nullptr);

    server.Stop();

    std::unique_ptr<TcpConn> refused;
    CHECK(DialTcp(kLoopback, port, kTimeoutUs, &refused) != COCO_SUCCESS);
    // The port is free again for the next server.
    TcpServer next(Echo);
    CHECK_EQ(next.Start(kLoopback, port), COCO_SUCCESS);
}

// Two coroutines stopping one server: whichever returns, the server is down by then. The
// second must not close the listener under the acceptor the first is still waiting for.
COTEST(TcpServerConcurrentStopsBothReturnWhenDown) {
    const int port = 19275;
    TcpServer server([](StreamConn &) {
        while (!CocoShouldStop()) {
            CocoSleepMs(5);
        }
        return COCO_SUCCESS;
    });
    CHECK_EQ(server.Start(kLoopback, port), COCO_SUCCESS);
    std::unique_ptr<TcpConn> client = Dial(port);
    CHECK(client != nullptr);
    CHECK(cotest::WaitUntil([&]() { return server.ConnCount() == 1; }));

    size_t conns[2] = {99, 99};
    bool refused[2] = {false, false};
    auto stop = [&](int i) {
        server.Stop();
        conns[i] = server.ConnCount();
        std::unique_ptr<TcpConn> c;
        refused[i] = DialTcp(kLoopback, port, kTimeoutUs, &c) != COCO_SUCCESS;
    };
    st_thread_t a = cotest::Go([&]() { stop(0); });
    st_thread_t b = cotest::Go([&]() { stop(1); });
    st_thread_join(a, nullptr);
    st_thread_join(b, nullptr);

    for (int i = 0; i < 2; ++i) {
        CHECK_EQ(conns[i], 0);
        CHECK(refused[i]);
    }
}

// What a shell does for Ctrl-C: the first signal ends the server gracefully, with exit
// status 0 and its connections closed.
COTEST(RuntimeSignalStopsServerProcess) {
    const int port = 19273;
    int out = -1;
    pid_t pid = SpawnHelper("HelperServeUntilSignal", &out);
    CHECK(pid > 0);
    if (pid <= 0) {
        return;
    }
    CHECK(WaitForReady(out));
    close(out);

    std::unique_ptr<TcpConn> c = Dial(port);
    CHECK(c != nullptr);
    CHECK(c != nullptr && Echoes(c.get(), "hi"));

    kill(pid, SIGTERM);
    if (c) {
        CHECK(PeerClosed(c.get()));
    }
    int status = 0;
    CHECK(WaitForExit(pid, 3000, &status));
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// With no coroutine running the first signal cannot be served, and shutting down may
// hang: a second one ends the process anyway.
COTEST(RuntimeSecondSignalEndsStuckProcess) {
    int out = -1;
    pid_t pid = SpawnHelper("HelperSpinInsideCocoRun", &out);
    CHECK(pid > 0);
    if (pid <= 0) {
        return;
    }
    CHECK(WaitForReady(out));
    close(out);

    kill(pid, SIGINT);
    usleep(100 * 1000);
    int status = 0;
    CHECK_EQ(waitpid(pid, &status, WNOHANG), 0);

    kill(pid, SIGINT);
    CHECK(WaitForExit(pid, 3000, &status));
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGINT);
}

// A signal that was ignored stays ignored, like for a job a shell started in the background.
COTEST(RuntimeIgnoredSignalStaysIgnored) {
    ShutdownGuard guard;
    struct sigaction ignore, old_int, current;
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    ignore.sa_flags = 0;
    sigaction(SIGINT, &ignore, &old_int);

    st_thread_t t = cotest::Go([]() {
        CocoSleepMs(10);
        CocoShutdown();
    });
    CocoWaitForShutdown();
    st_thread_join(t, nullptr);

    sigaction(SIGINT, nullptr, &current);
    CHECK(current.sa_handler == SIG_IGN);
    sigaction(SIGINT, &old_int, nullptr);
}

// The same with worker threads: the signal lands on any thread, the server's thread still
// stops the workers and their connections, and the process exits with status 0.
COTEST(RuntimeSignalStopsThreadedServerProcess) {
    const int port = 19276;
    int out = -1;
    pid_t pid = SpawnHelper("HelperServeThreadsUntilSignal", &out);
    CHECK(pid > 0);
    if (pid <= 0) {
        return;
    }
    CHECK(WaitForReady(out));
    close(out);

    std::unique_ptr<TcpConn> a = Dial(port);
    std::unique_ptr<TcpConn> b = Dial(port);
    CHECK(a != nullptr && Echoes(a.get(), "a"));
    CHECK(b != nullptr && Echoes(b.get(), "b"));

    kill(pid, SIGTERM);
    if (a) {
        CHECK(PeerClosed(a.get()));
    }
    if (b) {
        CHECK(PeerClosed(b.get()));
    }
    int status = 0;
    CHECK(WaitForExit(pid, 3000, &status));
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// A build with assertions stops a thread that uses another thread's object; a release
// build does not check.
COTEST(RuntimeOwnerCheckAbortsInDebug) {
#ifndef NDEBUG
    int out = -1;
    pid_t pid = SpawnHelper("HelperUseOtherThreadsObject", &out);
    CHECK(pid > 0);
    if (pid <= 0) {
        return;
    }
    close(out);
    int status = 0;
    CHECK(WaitForExit(pid, 3000, &status));
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
#endif
}

// The helper cases below run in a child process of the cases above; the runner skips them.
COTEST(HelperServeThreadsUntilSignal) {
    TcpServerOptions opt;
    opt.threads = 2;
    TcpServer server(Echo, opt);
    if (server.Start(kLoopback, 19276) != COCO_SUCCESS) {
        _exit(3);
    }
    fputs("ready\n", stdout);
    fflush(stdout);
    server.Wait();
}

COTEST(HelperUseOtherThreadsObject) {
    CocoThread worker;
    std::thread([&worker]() { worker.Stop(); }).join();
    _exit(0);
}

COTEST(HelperServeUntilSignal) {
    TcpServer server(Echo);
    if (server.Start(kLoopback, 19273) != COCO_SUCCESS) {
        _exit(3);
    }
    fputs("ready\n", stdout);
    fflush(stdout);
    server.Wait();
}

COTEST(HelperSpinInsideCocoRun) {
    CocoRun([]() {
        fputs("ready\n", stdout);
        fflush(stdout);
        // Never yields, so no coroutine, the watcher included, gets to run.
        volatile bool forever = true;
        while (forever) {
        }
        return 0;
    });
}
