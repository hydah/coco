#pragma once

#include <stddef.h>

#include <functional>
#include <memory>

namespace coco {

struct CocoThreadState;

// A kernel thread with a coroutine runtime of its own. Functions posted to it run there,
// each on a new coroutine, so they may block on I/O like any coroutine. The object belongs
// to the thread that created it; other threads may only call Post() and Load().
//
//   CocoThread worker;
//   if (worker.Start() != COCO_SUCCESS) return 1;
//   worker.Post([]() { /* runs on the worker, e.g. serves a connection */ });
//   ...
//   worker.Stop();  // interrupts what still runs, waits for it, joins the thread
//
// A function has to end when its coroutine is interrupted, as a server's handler does: the
// blocking call in progress fails and CocoShouldStop() turns true. That happens on Stop(),
// and to the functions running when a shutdown is requested (CocoShutdown(), SIGINT,
// SIGTERM); the thread itself keeps running until Stop().
class CocoThread {
 public:
    CocoThread();
    // Stop().
    ~CocoThread();

    CocoThread(const CocoThread &) = delete;
    CocoThread &operator=(const CocoThread &) = delete;

    // Spawns the thread and sets its runtime up; returns what CocoInit() returned there.
    // Functions posted before run once it has started. It can only be started once.
    int Start();

    // Thread-safe and never blocks. fn runs once, on a new coroutine on this thread; it is
    // destroyed without running only if no coroutine can be created for it. Fails with
    // ERROR_THREAD_DISPOSED, and fn never runs, once Stop() has started.
    int Post(std::function<void()> fn);

    // Stops taking posts, interrupts every function posted before, those not started yet
    // included, waits until all have returned, then joins the thread. Only the calling
    // coroutine waits; the other coroutines of the caller's thread keep running. A second
    // call waits for the first.
    void Stop();

    // Functions posted that have not returned yet. Thread-safe.
    size_t Load() const;

 private:
    // Shared with the thread, which may still use it while it returns.
    std::shared_ptr<CocoThreadState> state_;
};

}  // namespace coco
