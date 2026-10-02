#include <stdio.h>

#include <chrono>
#include <memory>
#include <thread>

#include "coco/coco.h"
#include "thread_name.hpp"

using namespace coco;

static int Echo(StreamConn &conn) {
    char buf[64];
    ssize_t n = 0;
    int ret;
    while ((ret = conn.Read(buf, sizeof(buf), &n)) == COCO_SUCCESS) {
        conn.Write(buf, n, nullptr);
    }
    return ret;
}

// Each thread runs its own server until the shutdown, checking it once with a client.
static void Serve(int port) {
    CocoRun([port]() {
        TcpServer server(Echo);
        if (server.Start("127.0.0.1", port) != COCO_SUCCESS) {
            return 1;
        }
        std::unique_ptr<TcpConn> c;
        char buf[8];
        ssize_t n = 0;
        if (DialTcp("127.0.0.1", port, 1000 * 1000, &c) == COCO_SUCCESS &&
            c->Write((void *)"ping", 4, nullptr) == COCO_SUCCESS &&
            c->ReadFully(buf, 4, &n) == COCO_SUCCESS) {
            printf("%s: serving on %d, echo ok\n", ThreadName(), port);
        }
        c.reset();
        server.Wait();  // until the shutdown, then the server stops
        printf("%s: server on %d stopped\n", ThreadName(), port);
        return 0;
    });
}

// Plain std::threads, each with CocoRun: two runtimes side by side that share nothing and
// hand each other nothing. The main thread never uses coco except for CocoShutdown(),
// which any thread may call, and which reaches both of them.
int main() {
    log_level = log_error;
    ThreadName();
    std::thread a(Serve, 8093);
    std::thread b(Serve, 8094);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    printf("%s: CocoShutdown()\n", ThreadName());
    CocoShutdown();
    a.join();
    b.join();
    return 0;
}
