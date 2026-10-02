#include <stdio.h>

#include <memory>
#include <string>
#include <vector>

#include "coco/coco.h"
#include "thread_name.hpp"

using namespace coco;

// A server on several cores: with TcpServerOptions::threads the main thread only accepts,
// and each connection runs on whichever of the four worker threads has the fewest. The
// handler replies with the name of the thread it runs on.
int main() {
    log_level = log_error;
    ThreadName();
    return CocoRun([]() {
        const int port = 8092;
        TcpServerOptions opt;
        opt.threads = 4;
        TcpServer server(
            [](StreamConn &conn) {
                char buf[64];
                ssize_t n = 0;
                while (conn.Read(buf, sizeof(buf), &n) == COCO_SUCCESS) {
                    // Called on several threads at once: it only uses its own locals.
                    std::string reply = ThreadName();
                    conn.Write((void *)reply.data(), reply.size(), nullptr);
                }
                return COCO_SUCCESS;
            },
            opt);
        if (server.Start("127.0.0.1", port) != COCO_SUCCESS) {
            return 1;
        }
        printf("accepting on %s, serving on 4 worker threads\n", ThreadName());

        // The connections stay open, so each new one goes to the least busy worker.
        std::vector<std::unique_ptr<TcpConn>> clients;
        for (int i = 0; i < 8; ++i) {
            std::unique_ptr<TcpConn> c;
            if (DialTcp("127.0.0.1", port, 1000 * 1000, &c) != COCO_SUCCESS) {
                return 1;
            }
            char buf[16];
            ssize_t n = 0;
            if (c->Write((void *)"who", 3, nullptr) != COCO_SUCCESS ||
                c->Read(buf, sizeof(buf), &n) != COCO_SUCCESS) {
                return 1;
            }
            printf("client %d: served on %.*s\n", i, (int)n, buf);
            clients.push_back(std::move(c));
        }
        printf("%zu connections open\n", server.ConnCount());

        // Stops accepting, then each worker ends its connections and its thread.
        server.Stop();
        printf("stopped, %zu connections open\n", server.ConnCount());
        return 0;
    });
}
