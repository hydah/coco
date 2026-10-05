#pragma once

#include <stddef.h>

#include <functional>
#include <unordered_set>

#include "coco/base/owner_thread.hpp"
#include "coco/base/st_fwd.hpp"
#include "coco/common/error.hpp"

namespace coco {

class GroupTask;

// Functions running side by side on the calling thread, each on a coroutine of its own,
// and stopped and waited for together. Nothing to derive from and nothing to delete: the
// group owns its coroutines, and its destructor cancels them and waits until all have
// returned, so they may use what lives on the stack of the scope that owns the group.
//
//   TaskGroup tasks;
//   tasks.Spawn([&]() { return ReadLoop(conn); });
//   tasks.Spawn([&]() { return Heartbeat(conn); });
//   CocoWaitForShutdown();
//   tasks.Cancel();
//   return tasks.Wait();
//
// A function has to end when its coroutine is interrupted: the blocking call in progress
// fails and CocoShouldStop() turns true. One that never blocks never sees the interrupt,
// nor anything else of its thread; a long loop calls CocoYield().
//
// The group belongs to the thread that created it; use it from that thread only.
class TaskGroup {
 public:
    TaskGroup();
    // Cancel(), then Wait().
    ~TaskGroup();

    TaskGroup(const TaskGroup &) = delete;
    TaskGroup &operator=(const TaskGroup &) = delete;

    // Starts fn on a new coroutine of the calling thread; it runs once the caller yields.
    // Fails, and fn is destroyed without running, when the coroutine cannot be created.
    // After Cancel() fn still runs, but interrupted from the start, as a CocoRun() body
    // started after a shutdown request is.
    int Spawn(std::function<int()> fn);

    // Interrupts every function running, and those spawned from now on. Never waits.
    void Cancel();

    // Suspends the calling coroutine until every function has returned, and returns the
    // first error one returned (ERROR_THREAD_INTERRUPED, typically, for those cancelled),
    // or COCO_SUCCESS. When the caller itself is interrupted, or CocoShouldStop() is true
    // for it, it cancels the group and goes on waiting, so a stop reaches the functions.
    // Waits for ever for one that ignores the interrupt, and must not be called from a
    // function of the group, which would wait for itself.
    int Wait();

    bool Cancelled() const { return cancelled_; }
    // Functions that have not returned yet.
    size_t Size() const { return tasks_.size(); }

 private:
    friend class GroupTask;
    void Done(GroupTask *t, int ret);

    std::unordered_set<GroupTask *> tasks_;
    // Broadcast when tasks_ becomes empty. Created with the first task.
    st_cond_t drained_ = nullptr;
    bool cancelled_ = false;
    int err_ = COCO_SUCCESS;
    OwnerThread owner_;
};

}  // namespace coco
