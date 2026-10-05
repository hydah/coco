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

#include "st.h"

#include "coco/base/coroutine.hpp"
#include "coco/base/owner_thread.hpp"
#include "coco/base/shutdown.hpp"
#include "coco/base/task_group.hpp"
#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/log/log.hpp"

namespace coco {

struct CocoThreadState {
    explicit CocoThreadState(size_t max) : max_load(max) {}

    // Shared with the threads that post.
    std::mutex mu;
    std::deque<std::function<void()>> queue;
    // Stop() has started: posts are refused, and the worker stops once the queue is empty.
    bool closed = false;
    // Wakes the worker; the write end is -1 before Start() and after Stop().
    int wake[2] = {-1, -1};
    std::atomic<size_t> load{0};
    // 0 for no limit.
    const size_t max_load;

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

// A posted function. It counts in Load() until it is destroyed, whether it ran or not.
class Posted {
 public:
    Posted(std::function<void()> fn, std::atomic<size_t> *load) : fn(std::move(fn)), load_(load) {
        ++*load_;
    }
    ~Posted() { --*load_; }

    Posted(const Posted &) = delete;
    Posted &operator=(const Posted &) = delete;

    std::function<void()> fn;

 private:
    std::atomic<size_t> *load_;
};

// Starts what is posted on a TaskGroup until Stop(), then cancels the group and waits for
// it. Only used on the worker thread.
void RunTasks(CocoThreadState *state, st_netfd_t wake) {
    TaskGroup tasks;
    // A shutdown request interrupts what runs, and what is started later starts
    // interrupted; the thread itself keeps running until Stop().
    SetShutdownHook([&tasks]() { tasks.Cancel(); });
    for (;;) {
        bool closed = false;
        {
            // Each task gets a copy, so the batch holds every function, and its place in
            // Load(), until it is gone, which has to be before the worker waits.
            std::deque<std::function<void()>> batch;
            {
                std::lock_guard<std::mutex> lock(state->mu);
                batch.swap(state->queue);
                closed = state->closed;
            }
            for (auto &fn : batch) {
                tasks.Spawn([fn]() {
                    fn();
                    return COCO_SUCCESS;
                });
            }
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
    tasks.Cancel();
    tasks.Wait();
    SetShutdownHook(nullptr);
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
        RunTasks(state.get(), wake);
        CloseNetfd(wake);
    } else {
        close(state->wake[0]);
    }

    char b = 0;
    ssize_t n = write(state->done[1], &b, 1);
    (void)n;
}

}  // namespace

CocoThread::CocoThread(size_t max_load) : state_(std::make_shared<CocoThreadState>(max_load)) {}

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
    // Only Post() adds to the load, under this lock, so the limit is never passed.
    if (s.max_load > 0 && s.load >= s.max_load) {
        return ERROR_THREAD_BUSY;
    }
    auto posted = std::make_shared<Posted>(std::move(fn), &s.load);
    s.queue.push_back([posted]() { posted->fn(); });
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

namespace {

// Closes the write end of a pipe once the posted function holding it is destroyed, which
// is the caller's cue that the function has returned, or will never run.
class CallDone {
 public:
    explicit CallDone(int fd) : fd_(fd) {}
    ~CallDone() {
        // The byte, not only the close, so that the reader's wake-up is ordered after the
        // function for ThreadSanitizer too.
        char b = 0;
        ssize_t n = write(fd_, &b, 1);
        (void)n;
        close(fd_);
    }

    CallDone(const CallDone &) = delete;
    CallDone &operator=(const CallDone &) = delete;

 private:
    int fd_;
};

}  // namespace

int CocoThread::Call(std::function<int()> fn) {
    if (!fn) {
        return ERROR_SYSTEM_ASSERT_FAILED;
    }
    int fds[2];
    if (!MakePipe(fds)) {
        coco_error("coco thread pipe failed. errno=%d", errno);
        return ERROR_SYSTEM_CREATE_PIPE;
    }
    auto result = std::make_shared<std::atomic<int>>(ERROR_THREAD_DISPOSED);
    auto done = std::make_shared<CallDone>(fds[1]);
    int ret = Post([fn, result, done]() { result->store(fn()); });
    // From here only the posted function holds the write end.
    done.reset();
    if (ret != COCO_SUCCESS) {
        CloseFd(&fds[0]);
        return ret;
    }
    // fn may use what lives on the caller's stack, so the wait goes on through interrupts.
    WaitReadable(&fds[0]);
    return result->load();
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
    // What was posted to a thread that never ran, as Start() was not called or failed.
    std::deque<std::function<void()>> unrun;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        CloseFd(&s.wake[1]);
        // Closed by the worker.
        s.wake[0] = -1;
        unrun.swap(s.queue);
    }
    // Outside the lock, as destroying what a function captured may post.
    unrun.clear();
    CloseFd(&s.done[0]);
    CloseFd(&s.done[1]);
    s.stopped = true;
    NotifyShutdownWaiters();
}

size_t CocoThread::Load() const { return state_->load; }

}  // namespace coco
