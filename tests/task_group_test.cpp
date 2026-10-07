// TaskGroup, CocoYield, and what CocoThread adds on top of Post(): Call(), a load limit,
// and the functions of a thread that never starts.

#include <atomic>
#include <memory>
#include <thread>

#include "st.h"

#include "coco/base/coco_thread.hpp"
#include "coco/base/shutdown.hpp"
#include "coco/base/task_group.hpp"
#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "test_util.hpp"

using namespace coco;

namespace {

long ElapsedMs(st_utime_t since) { return (long)((st_utime() - since) / 1000); }

// Sleeps until its coroutine is stopped.
int SleepUntilStopped() {
    while (!CocoShouldStop()) {
        CocoSleepMs(10 * 1000);
    }
    return COCO_SUCCESS;
}

struct ShutdownGuard {
    ~ShutdownGuard() { ResetShutdown(); }
};

}  // namespace

// The functions run side by side, and Wait() returns once the last one has.
COTEST(TaskGroupWaitsForAll) {
    TaskGroup tasks;
    std::atomic<int> done(0);
    st_utime_t begin = st_utime();
    for (int i = 1; i <= 3; ++i) {
        CHECK_EQ(tasks.Spawn([&done, i]() {
                     CocoSleepMs(50 * i);
                     ++done;
                     return COCO_SUCCESS;
                 }),
                 COCO_SUCCESS);
    }
    CHECK_EQ(tasks.Size(), 3);
    CHECK_EQ(tasks.Wait(), COCO_SUCCESS);
    CHECK_EQ(done.load(), 3);
    CHECK_EQ(tasks.Size(), 0);
    // Side by side: 150ms, not 300.
    CHECK(ElapsedMs(begin) < 250);
    CHECK(!tasks.Cancelled());
    CHECK_EQ(tasks.Spawn(nullptr), ERROR_SYSTEM_ASSERT_FAILED);
}

// Wait() returns the first error a function returned.
COTEST(TaskGroupWaitReturnsFirstError) {
    TaskGroup tasks;
    tasks.Spawn([]() { return COCO_SUCCESS; });
    tasks.Spawn([]() {
        CocoSleepMs(10);
        return ERROR_SOCKET_TIMEOUT;
    });
    tasks.Spawn([]() {
        CocoSleepMs(30);
        return ERROR_SOCKET_CLOSED;
    });
    CHECK_EQ(tasks.Wait(), ERROR_SOCKET_TIMEOUT);
}

// Cancel() interrupts every function without waiting; one spawned afterwards starts
// interrupted.
COTEST(TaskGroupCancelInterrupts) {
    TaskGroup tasks;
    std::atomic<int> stopped(0);
    for (int i = 0; i < 3; ++i) {
        tasks.Spawn([&stopped]() {
            SleepUntilStopped();
            ++stopped;
            return COCO_SUCCESS;
        });
    }
    CocoSleepMs(10);
    st_utime_t begin = st_utime();
    tasks.Cancel();
    CHECK(tasks.Cancelled());
    CHECK_EQ(stopped.load(), 0);

    bool stopped_at_start = false;
    int sleep_ret = 0;
    CHECK_EQ(tasks.Spawn([&]() {
                 stopped_at_start = CocoShouldStop();
                 // The interrupt fails the first blocking call only.
                 CocoSleepMs(10 * 1000);
                 st_utime_t t = st_utime();
                 CocoSleepMs(5);
                 sleep_ret = (int)ElapsedMs(t);
                 return COCO_SUCCESS;
             }),
             COCO_SUCCESS);
    CHECK_EQ(tasks.Wait(), COCO_SUCCESS);
    CHECK(ElapsedMs(begin) < 1000);
    CHECK_EQ(stopped.load(), 3);
    CHECK(stopped_at_start);
    CHECK(sleep_ret >= 4);
}

// The destructor cancels the functions and waits for them, so they may use the stack of
// the scope that owns the group.
COTEST(TaskGroupDestructorCancelsAndWaits) {
    int on_stack = 0;
    {
        TaskGroup tasks;
        tasks.Spawn([&on_stack]() {
            SleepUntilStopped();
            CocoSleepMs(20);  // cleaning up still yields
            on_stack = 1;
            return COCO_SUCCESS;
        });
        CocoSleepMs(5);
    }
    CHECK_EQ(on_stack, 1);
}

// An interrupt of the coroutine in Wait() cancels the group, even on a coroutine coco did
// not make, and the wait goes on until the functions have returned.
COTEST(TaskGroupWaitCancelsWhenInterrupted) {
    bool finished = false;
    int ret = -1;
    st_thread_t waiter = cotest::Go([&]() {
        TaskGroup tasks;
        tasks.Spawn([&finished]() {
            SleepUntilStopped();
            CocoSleepMs(20);
            finished = true;
            return ERROR_THREAD_INTERRUPED;
        });
        ret = tasks.Wait();
    });
    CocoSleepMs(10);
    st_thread_interrupt(waiter);
    st_thread_join(waiter, nullptr);
    CHECK(finished);
    CHECK_EQ(ret, ERROR_THREAD_INTERRUPED);

    TaskGroup tasks;
    st_thread_t parent = st_thread_self();
    CHECK_EQ(tasks.Spawn([parent]() {
                 st_thread_interrupt(parent);
                 return COCO_SUCCESS;
             }),
             COCO_SUCCESS);
    CHECK_EQ(tasks.Wait(), COCO_SUCCESS);
    CHECK(tasks.Cancelled());
}

// A shutdown stops a CocoRun() body waiting on a group, and through it the group.
COTEST(TaskGroupInCocoRunStopsOnShutdown) {
    ShutdownGuard guard;
    std::atomic<int> stopped(0);
    st_utime_t begin = st_utime();
    int ret = CocoRun([&]() {
        TaskGroup tasks;
        for (int i = 0; i < 2; ++i) {
            tasks.Spawn([&stopped]() {
                SleepUntilStopped();
                ++stopped;
                return COCO_SUCCESS;
            });
        }
        tasks.Spawn([]() {
            CocoSleepMs(20);
            CocoShutdown();
            return COCO_SUCCESS;
        });
        return tasks.Wait() == COCO_SUCCESS ? 7 : 1;
    });
    CHECK_EQ(ret, 7);
    CHECK_EQ(stopped.load(), 2);
    CHECK(ElapsedMs(begin) < 1000);
}

// A loop that does no I/O but yields lets the stop request in: Stop() reaches the worker's
// own coroutine through a pipe, which it only reads when the loop yields.
COTEST(CocoYieldLetsStopReachBusyLoop) {
    CocoThread worker;
    CHECK_EQ(worker.Start(), COCO_SUCCESS);
    std::atomic<bool> started(false), stopped(false);
    std::atomic<long> spins(0);
    worker.Post([&]() {
        started = true;
        while (!CocoShouldStop()) {
            ++spins;
            CocoYield();
        }
        stopped = true;
    });
    CHECK(cotest::WaitUntil([&]() { return started.load(); }));
    st_utime_t begin = st_utime();
    worker.Stop();
    CHECK(stopped);
    CHECK(spins > 0);
    CHECK(ElapsedMs(begin) < 1000);
}

// Functions posted to a thread that never starts are destroyed by Stop(), not kept until
// the object goes.
COTEST(CocoThreadNeverStartedDropsPosts) {
    CocoThread worker;
    auto token = std::make_shared<int>(0);
    bool ran = false;
    CHECK_EQ(worker.Post([token, &ran]() { ran = true; }), COCO_SUCCESS);
    CHECK_EQ(worker.Load(), 1);
    CHECK_EQ(token.use_count(), 2);
    worker.Stop();
    CHECK_EQ(worker.Load(), 0);
    CHECK_EQ(token.use_count(), 1);
    CHECK(!ran);
    CHECK_EQ(worker.Start(), ERROR_THREAD_DISPOSED);
}

// Call() returns what the function returned, once it has, on the worker.
COTEST(CocoThreadCallReturnsResult) {
    CocoThread worker;
    CHECK_EQ(worker.Start(), COCO_SUCCESS);
    std::thread::id where;
    int on_stack = 0;
    int ticks = 0;
    bool done = false;
    st_thread_t ticker = cotest::Go([&]() {
        while (!done) {
            ++ticks;
            CocoSleepMs(1);
        }
    });
    int ret = worker.Call([&]() {
        CocoSleepMs(50);
        where = std::this_thread::get_id();
        on_stack = 42;
        return ERROR_SOCKET_TIMEOUT;
    });
    done = true;
    st_thread_join(ticker, nullptr);
    CHECK_EQ(ret, ERROR_SOCKET_TIMEOUT);
    CHECK_EQ(on_stack, 42);
    CHECK(where != std::this_thread::get_id());
    // Only the caller waited.
    CHECK(ticks > 10);
    CHECK_EQ(worker.Load(), 0);
    CHECK_EQ(worker.Call(nullptr), ERROR_SYSTEM_ASSERT_FAILED);

    worker.Stop();
    CHECK_EQ(worker.Call([]() { return COCO_SUCCESS; }), ERROR_THREAD_DISPOSED);
}

// A thread with no runtime blocks in Call(); and a call to a thread that never starts
// returns once Stop() has dropped it.
COTEST(CocoThreadCallFromPlainThreadAndNeverStarted) {
    CocoThread worker;
    CHECK_EQ(worker.Start(), COCO_SUCCESS);
    int ret = -1;
    std::thread plain([&]() { ret = worker.Call([]() { return 5; }); });
    plain.join();
    CHECK_EQ(ret, 5);
    worker.Stop();

    CocoThread never_started;
    int never_ret = -1;
    bool ran = false;
    st_thread_t caller = cotest::Go([&]() {
        never_ret = never_started.Call([&ran]() {
            ran = true;
            return COCO_SUCCESS;
        });
    });
    CocoSleepMs(10);
    never_started.Stop();
    st_thread_join(caller, nullptr);
    CHECK_EQ(never_ret, ERROR_THREAD_DISPOSED);
    CHECK(!ran);
}

// At the limit Post() and Call() fail with ERROR_THREAD_BUSY; a slot frees when a function
// returns.
COTEST(CocoThreadMaxLoad) {
    CocoThread worker(2);
    CHECK_EQ(worker.Start(), COCO_SUCCESS);
    std::atomic<bool> release(false);
    for (int i = 0; i < 2; ++i) {
        CHECK_EQ(worker.Post([&release]() {
                     while (!release && !CocoShouldStop()) {
                         CocoSleepMs(1);
                     }
                 }),
                 COCO_SUCCESS);
    }
    CHECK_EQ(worker.Load(), 2);
    bool ran = false;
    CHECK_EQ(worker.Post([&ran]() { ran = true; }), ERROR_THREAD_BUSY);
    CHECK_EQ(worker.Call([]() { return COCO_SUCCESS; }), ERROR_THREAD_BUSY);
    release = true;
    CHECK(cotest::WaitUntil([&]() { return worker.Load() == 0; }));
    CHECK_EQ(worker.Call([]() { return 3; }), 3);
    worker.Stop();
    CHECK(!ran);
}

// Once a shutdown is requested, a function posted later starts interrupted, as a CocoRun()
// body started after the request does; the worker keeps running it.
COTEST(CocoThreadPostAfterShutdownStartsInterrupted) {
    ShutdownGuard guard;
    CocoThread worker;
    CHECK_EQ(worker.Start(), COCO_SUCCESS);
    CHECK_EQ(worker.Call([]() { return COCO_SUCCESS; }), COCO_SUCCESS);
    CocoShutdown();
    // The worker serves the request when its notifier runs; a function posted after that
    // starts interrupted.
    CHECK(cotest::WaitUntil([&]() {
        return worker.Call([]() { return CocoShouldStop() ? 1 : 0; }) == 1;
    }));
    worker.Stop();
}
