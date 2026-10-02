#include "coco/base/coco_thread.hpp"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <deque>
#include <future>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#include "st.h"

#include "coco/base/coroutine.hpp"
#include "coco/base/owner_thread.hpp"
#include "coco/base/shutdown.hpp"
#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/log/log.hpp"

namespace coco {

struct CocoThreadState {
    // Shared with the threads that post.
    std::mutex mu;
    std::deque<std::function<void()>> queue;
    // Stop() has started: posts are refused, and the worker stops once the queue is empty.
    bool closed = false;
    // Wakes the worker; the write end is -1 before Start() and after Stop().
    int wake[2] = {-1, -1};
    std::atomic<size_t> load{0};

    // Only touched by the owner.
    OwnerThread owner;
    std::thread thread;
    // The worker writes one byte to it when it is done, which Stop() waits for.
    int done[2] = {-1, -1};
    bool started = false;
    bool stopping = false;
    bool stopped = false;
};

namespace {

class Worker;

// One posted function, on a detached coroutine that deletes the task when it returns.
class Task : public CoroutineHandler {
 public:
    Task(Worker *worker, std::function<void()> fn) : worker_(worker), fn_(std::move(fn)) {
        coroutine = new CoCoroutine("task", this);
        coroutine->set_detached(true);
    }
    ~Task() override;

    int Start() { return coroutine->start(); }
    void Interrupt() { coroutine->interrupt(); }
    int Cycle() override {
        fn_();
        return COCO_SUCCESS;
    }

 private:
    Worker *worker_;
    std::function<void()> fn_;
};

// What the worker thread keeps while it runs. Only used on that thread.
class Worker {
 public:
    explicit Worker(CocoThreadState *state) : state_(state), drained_(st_cond_new()) {}
    ~Worker() { st_cond_destroy(drained_); }

    // Starts what is posted until Stop(), then interrupts every task and waits for them.
    void Run(st_netfd_t wake) {
        for (;;) {
            std::deque<std::function<void()>> batch;
            bool closed = false;
            {
                std::lock_guard<std::mutex> lock(state_->mu);
                batch.swap(state_->queue);
                closed = state_->closed;
            }
            for (auto &fn : batch) {
                StartTask(std::move(fn));
            }
            if (closed) {
                break;
            }
            char buf[64];
            ssize_t n = st_read(wake, buf, sizeof(buf), ST_UTIME_NO_TIMEOUT);
            if (n <= 0 && !(n < 0 && errno == EINTR)) {
                coco_error("coco thread wake pipe read failed. n=%d errno=%d", (int)n, errno);
                // Never happens; poll rather than spin.
                st_usleep(10 * 1000);
            }
        }
        InterruptAll();
        while (!tasks_.empty()) {
            st_cond_wait(drained_);
        }
    }

    void InterruptAll() {
        std::vector<Task *> live(tasks_.begin(), tasks_.end());
        for (Task *t : live) {
            t->Interrupt();
        }
    }

    void Done(Task *t) {
        tasks_.erase(t);
        --state_->load;
        if (tasks_.empty()) {
            st_cond_broadcast(drained_);
        }
    }

 private:
    void StartTask(std::function<void()> fn) {
        Task *t = new Task(this, std::move(fn));
        tasks_.insert(t);
        if (t->Start() != COCO_SUCCESS) {
            coco_error("coco thread could not start a task");
            delete t;
        }
    }

    CocoThreadState *state_;
    std::unordered_set<Task *> tasks_;
    st_cond_t drained_;
};

Task::~Task() {
    delete coroutine;
    worker_->Done(this);
}

void CloseFd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

bool MakePipe(int fds[2]) {
    if (pipe(fds) != 0) {
        return false;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
}

// The thread's body. It owns the read end of the wake pipe, and tells Stop() through the
// done pipe when it is about to return.
void RunWorker(std::shared_ptr<CocoThreadState> state, std::promise<int> ready) {
    int ret = CocoInit();
    st_netfd_t wake = ret == COCO_SUCCESS ? st_netfd_open(state->wake[0]) : nullptr;
    if (ret == COCO_SUCCESS && wake == nullptr) {
        coco_error("coco thread wake pipe failed. errno=%d", errno);
        ret = ERROR_ST_OPEN_SOCKET;
    }
    ready.set_value(ret);

    if (ret == COCO_SUCCESS) {
        {
            Worker worker(state.get());
            SetShutdownHook([&worker]() { worker.InterruptAll(); });
            worker.Run(wake);
            SetShutdownHook(nullptr);
        }
        CloseNetfd(wake);
    } else {
        close(state->wake[0]);
    }

    char b = 0;
    ssize_t n = write(state->done[1], &b, 1);
    (void)n;
}

}  // namespace

CocoThread::CocoThread() : state_(std::make_shared<CocoThreadState>()) {}

CocoThread::~CocoThread() { Stop(); }

int CocoThread::Start() {
    CocoThreadState &s = *state_;
    s.owner.Check();
    if (s.started) {
        return ERROR_THREAD_STARTED;
    }
    if (s.stopping) {
        return ERROR_THREAD_DISPOSED;
    }

    int wake[2];
    if (!MakePipe(wake)) {
        coco_error("coco thread pipe failed. errno=%d", errno);
        return ERROR_SYSTEM_CREATE_PIPE;
    }
    if (!MakePipe(s.done)) {
        coco_error("coco thread pipe failed. errno=%d", errno);
        CloseFd(&wake[0]);
        CloseFd(&wake[1]);
        return ERROR_SYSTEM_CREATE_PIPE;
    }
    // A full pipe already holds a wake-up, so Post() never has to wait.
    fcntl(wake[1], F_SETFL, fcntl(wake[1], F_GETFL) | O_NONBLOCK);
    {
        std::lock_guard<std::mutex> lock(s.mu);
        s.wake[0] = wake[0];
        s.wake[1] = wake[1];
    }

    std::promise<int> ready;
    std::future<int> result = ready.get_future();
    int ret = COCO_SUCCESS;
    try {
        s.thread = std::thread(RunWorker, state_, std::move(ready));
        // Only as long as the new thread takes to set its runtime up.
        ret = result.get();
    } catch (const std::system_error &e) {
        coco_error("coco thread could not be created: %s", e.what());
        ret = ERROR_ST_CREATE_CYCLE_THREAD;
        CloseFd(&s.wake[0]);
    }
    s.started = true;
    if (ret != COCO_SUCCESS) {
        Stop();
    }
    return ret;
}

int CocoThread::Post(std::function<void()> fn) {
    if (!fn) {
        return ERROR_SYSTEM_ASSERT_FAILED;
    }
    CocoThreadState &s = *state_;
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.closed) {
        return ERROR_THREAD_DISPOSED;
    }
    s.queue.push_back(std::move(fn));
    ++s.load;
    if (s.wake[1] >= 0) {
        char b = 0;
        ssize_t n = write(s.wake[1], &b, 1);
        (void)n;
    }
    return COCO_SUCCESS;
}

// Suspends the calling coroutine until fd becomes readable, or blocks the thread when it
// has no runtime to wait on. Closes fd.
static void WaitReadable(int *fd) {
    if (CocoRuntimeReady()) {
        st_netfd_t f = st_netfd_open(*fd);
        if (f != nullptr) {
            char b;
            // Only returning lets the caller join, so an interrupt does not end the wait.
            while (st_read(f, &b, 1, ST_UTIME_NO_TIMEOUT) < 0 && errno == EINTR) {
            }
            CloseNetfd(f);
            *fd = -1;
            return;
        }
    }
    char b;
    while (read(*fd, &b, 1) < 0 && errno == EINTR) {
    }
    CloseFd(fd);
}

void CocoThread::Stop() {
    CocoThreadState &s = *state_;
    s.owner.Check();
    if (s.stopping) {
        // Another coroutine is stopping the thread; returning before it is joined would
        // break the promise that it is gone.
        if (!s.stopped) {
            WaitUntilNotified([&s]() { return s.stopped; });
        }
        return;
    }
    s.stopping = true;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        s.closed = true;
        if (s.wake[1] >= 0) {
            char b = 0;
            ssize_t n = write(s.wake[1], &b, 1);
            (void)n;
        }
    }

    if (s.thread.joinable()) {
        WaitReadable(&s.done[0]);
        s.thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(s.mu);
        CloseFd(&s.wake[1]);
        // Closed by the worker.
        s.wake[0] = -1;
    }
    CloseFd(&s.done[0]);
    CloseFd(&s.done[1]);
    s.stopped = true;
    NotifyShutdownWaiters();
}

size_t CocoThread::Load() const { return state_->load; }

}  // namespace coco
