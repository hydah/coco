#include <stdlib.h>

#include <iostream>

#include "coco/coco.h"

using namespace coco;

// An echo server over RUDP: the handler is the same as for TCP, only the listener differs.
//
//   rudp_echo_server [port]          (default 9000, on 127.0.0.1)
int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 9000;
    return CocoRun([port]() {
        std::unique_ptr<RudpListener> l;
        int ret = ListenRudp("127.0.0.1", port, &l);
        if (ret != COCO_SUCCESS) {
            std::cerr << "listen failed: " << ret << std::endl;
            return 1;
        }
        std::cout << "rudp echo on " << l->Addr() << std::endl;
        TcpServer server([](StreamConn &conn) {
            char buf[4096];
            ssize_t n = 0;
            int ret;
            while ((ret = conn.Read(buf, sizeof(buf), &n)) == COCO_SUCCESS) {
                if ((ret = conn.Write(buf, (size_t)n, nullptr)) != COCO_SUCCESS) {
                    break;
                }
            }
            return ret;
        });
        return server.Serve(std::move(l)) == COCO_SUCCESS ? 0 : 1;  // until Ctrl-C
    });
}
