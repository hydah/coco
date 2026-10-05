#include <stdio.h>

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

// The main thread ticks every 50ms while a second coroutine, on the third tick, has 300ms
// of computation done. Run inline it stalls the main thread, and every coroutine on it,
// until it is done; called on a CocoThread it runs on the worker while the ticks go on,
// and only the coroutine that called waits for the result.
static void Ticks(CocoThread *worker) {
    int64_t begin = NowMs();
    bool done = false;
    TaskGroup tasks;
    tasks.Spawn([&]() {
        CocoSleepMs(100);
        long loops = 0;
        if (worker == nullptr) {
            loops = Compute(300);
        } else if (worker->Call([&loops, begin]() {
                       loops = Compute(300);
                       printf("  computed on %s at %lldms\n", ThreadName(),
                              (long long)(NowMs() - begin));
                       return COCO_SUCCESS;
                   }) != COCO_SUCCESS) {
            return 1;
        }
        printf("  %ld loops, back on %s at %lldms\n", loops, ThreadName(),
               (long long)(NowMs() - begin));
        done = true;
        return 0;
    });
    for (int tick = 0; tick < 8 || !done; ++tick) {
        printf("  tick %d on %s at %lldms\n", tick, ThreadName(), (long long)(NowMs() - begin));
        CocoSleepMs(50);
    }
    tasks.Wait();
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
        printf("called on a CocoThread: the ticks go on\n");
        Ticks(&worker);
        // Interrupts what still runs on the worker, waits for it and joins the thread.
        worker.Stop();
        return 0;
    });
}
