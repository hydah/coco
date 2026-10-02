#include <stdio.h>

#include <memory>
#include <string>
#include <vector>

#include "coco/coco.h"
#include "thread_name.hpp"

using namespace coco;

// One thread, many coroutines: CocoRun on the main thread, a server and three clients.
// Every line says T0, and the three replies, 100ms each, overlap instead of adding up,
// because each connection waits on its own coroutine.
int main() {
    log_level = log_error;
    ThreadName();
    return CocoRun([]() {
        const int port = 8091;
        TcpServer server([](StreamConn &conn) {
            char buf[64];
            ssize_t n = 0;
            while (conn.Read(buf, sizeof(buf), &n) == COCO_SUCCESS) {
                printf("  server on %s: got \"%.*s\", replies in 100ms\n", ThreadName(), (int)n,
                       buf);
                CocoSleepMs(100);  // yields: the other connections go on meanwhile
                conn.Write(buf, n, nullptr);
            }
            return COCO_SUCCESS;
        });
        if (server.Start("127.0.0.1", port) != COCO_SUCCESS) {
            return 1;
        }

        int64_t begin = NowMs();
        std::vector<std::unique_ptr<TcpConn>> clients;
        for (int i = 0; i < 3; ++i) {
            std::unique_ptr<TcpConn> c;
            if (DialTcp("127.0.0.1", port, 1000 * 1000, &c) != COCO_SUCCESS) {
                return 1;
            }
            std::string msg = "hello " + std::to_string(i);
            c->Write((void *)msg.data(), msg.size(), nullptr);
            clients.push_back(std::move(c));
        }
        for (size_t i = 0; i < clients.size(); ++i) {
            char buf[64];
            ssize_t n = 0;
            if (clients[i]->Read(buf, sizeof(buf), &n) != COCO_SUCCESS) {
                return 1;
            }
            printf("client %zu on %s: reply after %lldms\n", i, ThreadName(),
                   (long long)(NowMs() - begin));
        }
        printf("3 replies of 100ms took %lldms on one thread\n", (long long)(NowMs() - begin));
        return 0;
    });
}
