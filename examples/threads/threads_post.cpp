#include <stdio.h>

#include <atomic>

#include "coco/coco.h"
#include "thread_name.hpp"

using namespace coco;

// Burns the CPU for ms milliseconds without yielding, like encoding or parsing would.
static long Compute(int ms) {
    long loops = 0;
    int64_t end = NowMs() + ms;
    while (NowMs() < end) {
        ++loops;
    }
    return loops;
}

// The main thread ticks every 50ms, and on the third tick starts 300ms of computation.
// Run inline it stalls the main thread, and every coroutine on it, until it is done; posted
// to a CocoThread it runs on the worker while the ticks go on.
static void Ticks(CocoThread *worker) {
    std::atomic<bool> done(false);
    int64_t begin = NowMs();
    for (int tick = 0; tick < 8 || !done; ++tick) {
        printf("  tick %d on %s at %lldms\n", tick, ThreadName(), (long long)(NowMs() - begin));
        if (tick == 2) {
            if (worker == nullptr) {
                Compute(300);
                printf("  computed inline on %s at %lldms\n", ThreadName(),
                       (long long)(NowMs() - begin));
                done = true;
            } else {
                worker->Post([&done, begin]() {
                    Compute(300);
                    printf("  computed on %s at %lldms\n", ThreadName(),
                           (long long)(NowMs() - begin));
                    done = true;
                });
            }
        }
        CocoSleepMs(50);
    }
}

int main() {
    log_level = log_error;
    ThreadName();
    return CocoRun([]() {
        printf("inline: the ticks stop for 300ms\n");
        Ticks(nullptr);

        CocoThread worker;
        if (worker.Start() != COCO_SUCCESS) {
            return 1;
        }
        printf("posted to a CocoThread: the ticks go on\n");
        Ticks(&worker);
        // Interrupts what still runs on the worker, waits for it and joins the thread.
        worker.Stop();
        return 0;
    });
}
