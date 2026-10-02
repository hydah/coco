// Built against an installed coco by test-local.sh, once through find_package and once
// through pkg-config. Exits 0 when the library works from the outside.

#include <atomic>

#include "coco/coco.h"

using namespace coco;

int main() {
    return CocoRun([]() {
        CocoThread worker;
        if (worker.Start() != COCO_SUCCESS) {
            return 1;
        }
        std::atomic<bool> ran(false);
        worker.Post([&ran]() {
            CocoSleepMs(1);
            ran = true;
        });
        for (int i = 0; i < 1000 && !ran; ++i) {
            CocoSleepMs(1);
        }
        worker.Stop();

        std::unique_ptr<TcpConn> conn;
        return ran && TcpConnFromFd(-1, &conn) != COCO_SUCCESS ? 0 : 1;
    });
}
