#include "coco/base/task_group.hpp"

#include <vector>

#include "st.h"

#include "coco/base/coroutine.hpp"
#include "coco/coco_api.h"
#include "coco/log/log.hpp"

namespace coco {

// One function of a group, on a detached coroutine that deletes the task when it returns.
class GroupTask : public CoroutineHandler {
 public:
    GroupTask(TaskGroup *group, std::function<int()> fn) : group_(group), fn_(std::move(fn)) {
        coroutine = new CoCoroutine("task", this);
        coroutine->set_detached(true);
    }
    ~GroupTask() override {
        // What fn captured goes before Wait() can return, even if destroying it yields.
        fn_ = nullptr;
        delete coroutine;
        // Last use of the group: a waiting Wait() may free it once this task yields.
        group_->Done(this, ret_);
    }

    int Start() { return coroutine->start(); }
    void Interrupt() { coroutine->interrupt(); }
    int Cycle() override {
        ret_ = fn_();
        return COCO_SUCCESS;
    }

 private:
    TaskGroup *group_;
    std::function<int()> fn_;
    int ret_ = COCO_SUCCESS;
};

TaskGroup::TaskGroup() = default;

TaskGroup::~TaskGroup() {
    Cancel();
    Wait();
    if (drained_ != nullptr) {
        st_cond_destroy(drained_);
    }
}

int TaskGroup::Spawn(std::function<int()> fn) {
    owner_.Check();
    if (!fn) {
        return ERROR_SYSTEM_ASSERT_FAILED;
    }
    int ret = CocoInit();
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    if (drained_ == nullptr && (drained_ = st_cond_new()) == nullptr) {
        return ERROR_ST_CREATE_CYCLE_THREAD;
    }
    GroupTask *t = new GroupTask(this, std::move(fn));
    tasks_.insert(t);
    if ((ret = t->Start()) != COCO_SUCCESS) {
        coco_error("task group could not start a task. ret=%d", ret);
        delete t;
        return ret;
    }
    if (cancelled_) {
        t->Interrupt();
    }
    return COCO_SUCCESS;
}

void TaskGroup::Cancel() {
    owner_.Check();
    cancelled_ = true;
    // Interrupting never yields, so no task leaves the set meanwhile; the copy is only
    // because it is a set being walked.
    std::vector<GroupTask *> live(tasks_.begin(), tasks_.end());
    for (GroupTask *t : live) {
        t->Interrupt();
    }
}

int TaskGroup::Wait() {
    owner_.Check();
    // No yield between the check and st_cond_wait, so the last Done() cannot be missed.
    bool stop = CocoShouldStop();
    while (!tasks_.empty()) {
        if (stop && !cancelled_) {
            Cancel();
        }
        // Fails only when the caller is interrupted, which may not show in CocoShouldStop()
        // on a coroutine coco did not make.
        if (st_cond_wait(drained_) != 0) {
            stop = true;
        }
    }
    if (stop && !cancelled_) {
        Cancel();
    }
    return err_;
}

void TaskGroup::Done(GroupTask *t, int ret) {
    tasks_.erase(t);
    if (ret != COCO_SUCCESS && err_ == COCO_SUCCESS) {
        err_ = ret;
    }
    if (tasks_.empty()) {
        st_cond_broadcast(drained_);
    }
}

}  // namespace coco
