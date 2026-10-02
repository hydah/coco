#pragma once

#include <functional>

namespace coco {

// A shutdown request is for the whole process and may come from any thread. Everything
// else here is about the calling thread's runtime: its waiters, its CocoRun().

// Blocks the calling coroutine until done() holds, a shutdown is requested or the
// coroutine is interrupted. done() is checked again on every NotifyShutdownWaiters().
// Arms the SIGINT and SIGTERM handlers first, like CocoWaitForShutdown().
void WaitForShutdownOr(const std::function<bool()> &done);
// Blocks the calling coroutine until done() holds, whatever happens meanwhile: neither a
// shutdown request nor an interrupt of the coroutine ends it early. done() is checked
// again on every NotifyShutdownWaiters().
void WaitUntilNotified(const std::function<bool()> &done);
// Makes every waiter of this thread check its condition again.
void NotifyShutdownWaiters();
// Forgets a requested shutdown, so waiting can start over; the signals are armed again by
// the next wait.
void ResetShutdown();
// True on the coroutine CocoRun() runs its function on, once a shutdown was requested.
bool RunBodyShouldStop();
// Runs fn on this thread's runtime whenever a shutdown is requested, from whichever
// thread; nullptr removes it. One per thread.
void SetShutdownHook(std::function<void()> fn);

// Part of CocoInit(): lets other threads reach this thread when they request a shutdown.
int StartThreadShutdown();

}  // namespace coco
