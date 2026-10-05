#pragma once

#include <stddef.h>

#include <functional>
#include <memory>

namespace coco {

struct CocoThreadState;

// A kernel thread with a coroutine runtime of its own. Functions posted to it run there,
// each on a new coroutine, so they may block on I/O like any coroutine. The object belongs
// to the thread that created it; other threads may only call Post(), Call() and Load().
//
//   CocoThread worker;
//   if (worker.Start() != COCO_SUCCESS) return 1;
//   worker.Post([]() { /* runs on the worker, e.g. serves a connection */ });
//   ...
//   worker.Stop();  // interrupts what still runs, waits for it, joins the thread
//
// The functions run side by side, not one after the other: each has its coroutine, and one
// that blocks lets the others run. A function has to end when its coroutine is
// interrupted, as a server's handler does: the blocking call in progress fails and
// CocoShouldStop() turns true. That happens on Stop(), and once a shutdown is requested
// (CocoShutdown(), SIGINT, SIGTERM) to the functions running then and to those that start
// later, which start interrupted; the thread itself keeps running until Stop(). Only a
// function that blocks or calls CocoYield() sees either.
//
// What a function captures crosses threads with it; a socket or anything else coco made
// still belongs to the thread that made it (only TcpConn::Release() moves a connection).
class CocoThread {
 public:
    // At most max_load functions posted and not returned yet; 0 for no limit.
    explicit CocoThread(size_t max_load = 0);
    // Stop().
    ~CocoThread();

    CocoThread(const CocoThread &) = delete;
    CocoThread &operator=(const CocoThread &) = delete;

    // Spawns the thread and sets its runtime up; returns what CocoInit() returned there.
    // Functions posted before run once it has started. It can only be started once.
    int Start();

    // Thread-safe and never blocks. fn runs once, on a new coroutine on this thread; it is
    // destroyed without running if no coroutine can be created for it, or if the thread
    // never starts. Fails, and fn never runs, with ERROR_THREAD_DISPOSED once Stop() has
    // started, and with ERROR_THREAD_BUSY while Load() is at the limit. Success only
    // means fn is queued: to learn when it returns and what, use Call().
    int Post(std::function<void()> fn);

    // Posts fn and suspends the calling coroutine, or blocks the calling thread if it has
    // no runtime, until fn has returned; returns what fn returned, what Post() failed
    // with, or ERROR_THREAD_DISPOSED when fn never ran. fn may use the caller's stack, so
    // an interrupt of the caller does not end the wait. Takes a pipe per call, two fds
    // while it lasts: for a request now and then, not for every message.
    int Call(std::function<int()> fn);

    // Stops taking posts, interrupts every function posted before, those not started yet
    // included, waits until all have returned, then joins the thread. Only the calling
    // coroutine waits; the other coroutines of the caller's thread keep running. A second
    // call waits for the first. There is no timeout: it waits for ever for a function
    // that ignores the interrupt. Functions posted to a thread that never started are
    // destroyed without running.
    void Stop();

    // Functions posted that have not returned yet, or been destroyed. Thread-safe.
    size_t Load() const;

 private:
    // Shared with the thread, which may still use it while it returns.
    std::shared_ptr<CocoThreadState> state_;
};

}  // namespace coco
