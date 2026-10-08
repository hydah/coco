#include <stdlib.h>

#include <iostream>
#include <string>

#include "coco/coco.h"

using namespace coco;

// Sends bytes to an RUDP echo server, reads them back, closes, and prints the
// connection's statistics.
//
//   rudp_echo_client [host] [port] [bytes]     (default 127.0.0.1 9000 1048576)
int main(int argc, char **argv) {
    std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 9000;
    size_t size = argc > 3 ? (size_t)atol(argv[3]) : 1024 * 1024;
    return CocoRun([&]() {
        std::unique_ptr<RudpConn> conn;
        int ret = DialRudp(host, port, 3 * 1000 * 1000, &conn);
        if (ret != COCO_SUCCESS) {
            std::cerr << "dial failed: " << ret << std::endl;
            return 1;
        }
        std::string data(size, 'x');
        TaskGroup tasks;
        tasks.Spawn([&]() { return conn->Write((void *)data.data(), data.size(), nullptr); });
        size_t got = 0;
        char buf[8192];
        while (got < size && ret == COCO_SUCCESS) {
            ssize_t n = 0;
            if ((ret = conn->Read(buf, sizeof(buf), &n)) == COCO_SUCCESS) {
                got += (size_t)n;
            }
        }
        int wret = tasks.Wait();
        int cret = conn->Close();
        RudpStats s = conn->Stats();
        std::cout << "echoed " << got << " of " << size << " bytes, read " << ret << ", write "
                  << wret << ", close " << cret << std::endl
                  << "sent " << s.segments_sent << " segments, resent " << s.rto_resends
                  << " on timeout and " << s.fast_resends << " fast, srtt " << s.srtt_us
                  << "us, rto " << s.rto_us << "us, cwnd " << s.cwnd << std::endl;
        return got == size && cret == COCO_SUCCESS ? 0 : 1;
    });
}
