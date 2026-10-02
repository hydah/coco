#include "coco/base/shutdown.hpp"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "st.h"

#include "coco/base/coroutine.hpp"
#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/log/log.hpp"

namespace coco {

static std::atomic<bool> shutdown_requested(false);
// The signal that asked for it, 0 for CocoShutdown().
static std::atomic<int> shutdown_signal(0);

// What a thread with a runtime keeps. Never freed, as a log line or a static destructor
// may still use it while the thread exits.
struct ThreadShutdown {
    // Broadcast whenever a waiter's condition may have changed.
    st_cond_t waiters = nullptr;
    // The coroutine CocoRun() runs its function on, and whether a shutdown was requested
    // while it does.
    st_thread_t run_thread = nullptr;
    bool run_stop = false;
    std::function<void()> hook;
};
static thread_local ThreadShutdown *local_state = nullptr;

static ThreadShutdown &Local() {
    if (local_state == nullptr) {
        local_state = new ThreadShutdown();
    }
    return *local_state;
}

// The write ends of the pipes that wake each thread with a runtime. Never destroyed: the
// signal watcher may still use it while the process exits.
struct WakeRegistry {
    std::mutex mu;
    std::vector<int> fds;
};
static WakeRegistry &Registry() {
    static WakeRegistry *r = new WakeRegistry();
    return *r;
}

// Takes the thread off the registry when it exits.
struct WakeRegistration {
    int fd = -1;
    ~WakeRegistration() {
        if (fd < 0) {
            return;
        }
        WakeRegistry &r = Registry();
        std::lock_guard<std::mutex> lock(r.mu);
        r.fds.erase(std::remove(r.fds.begin(), r.fds.end(), fd), r.fds.end());
        close(fd);
        fd = -1;
    }
};
static thread_local WakeRegistration registration;

// What a shutdown request does on the thread it reaches. Runs on that thread.
static void ServeShutdownHere() {
    ThreadShutdown &t = Local();
    if (t.run_thread != nullptr && !t.run_stop) {
        t.run_stop = true;
        // Ends the blocking call CocoRun()'s function is in. The caller itself is running,
        // not blocked, so there is nothing to end.
        if (t.run_thread != st_thread_self()) {
            st_thread_interrupt(t.run_thread);
        }
    }
    if (t.hook) {
        t.hook();
    }
    NotifyShutdownWaiters();
}

static void *ShutdownNotifier(void *arg) {
    st_netfd_t rfd = static_cast<st_netfd_t>(arg);
    for (;;) {
        char buf[64];
        ssize_t n = st_read(rfd, buf, sizeof(buf), ST_UTIME_NO_TIMEOUT);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            coco_error("shutdown pipe read failed. n=%d errno=%d", (int)n, errno);
            return nullptr;
        }
        if (shutdown_requested) {
            ServeShutdownHere();
        }
    }
}

static void CloseOnExec(int fd) { fcntl(fd, F_SETFD, FD_CLOEXEC); }

static void SetNonBlocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

int StartThreadShutdown() {
    int fds[2];
    if (pipe(fds) != 0) {
        coco_error("shutdown pipe failed. errno=%d", errno);
        return ERROR_SYSTEM_CREATE_PIPE;
    }
    CloseOnExec(fds[0]);
    CloseOnExec(fds[1]);
    // A full pipe already holds a wake-up, so a writer never has to wait.
    SetNonBlocking(fds[1]);

    st_netfd_t rfd = st_netfd_open(fds[0]);
    if (rfd == nullptr || st_thread_create(ShutdownNotifier, rfd, 0, 0) == nullptr) {
        coco_error("shutdown notifier failed. errno=%d", errno);
        if (rfd != nullptr) {
            CloseNetfd(rfd);
        } else {
            close(fds[0]);
        }
        close(fds[1]);
        return ERROR_ST_CREATE_CYCLE_THREAD;
    }

    WakeRegistry &r = Registry();
    std::lock_guard<std::mutex> lock(r.mu);
    r.fds.push_back(fds[1]);
    registration.fd = fds[1];
    return COCO_SUCCESS;
}

static void WakeOtherThreads() {
    WakeRegistry &r = Registry();
    std::lock_guard<std::mutex> lock(r.mu);
    for (int fd : r.fds) {
        if (fd == registration.fd) {
            continue;
        }
        char b = 0;
        ssize_t n = write(fd, &b, 1);
        (void)n;
    }
}

// SIGINT and SIGTERM request a shutdown. The handler only writes the signal number to a
// pipe; SignalWatcher, a plain thread, reads it and calls CocoShutdown(), so no coroutine
// code runs in a signal handler, and the request reaches every thread whichever one the
// signal lands on.
static const int kSignals[] = {SIGINT, SIGTERM};
static const size_t kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);
static struct sigaction saved_actions[kSignalCount];
static int signal_pipe[2] = {-1, -1};
static bool watcher_tried = false;
static bool watcher_ok = false;
static bool signals_armed = false;
// Signals the handler has taken since it was armed.
static volatile sig_atomic_t signals_seen = 0;

// Guards the arming state above, which every thread's waits and the watcher touch.
static std::mutex &SignalMutex() {
    static std::mutex *m = new std::mutex();
    return *m;
}

static void OnSignal(int sig) {
    int saved = errno;
    if (signals_seen++ == 0) {
        unsigned char b = (unsigned char)sig;
        // The pipe is non-blocking; a full one only means signals are pending anyway.
        ssize_t n = write(signal_pipe[1], &b, 1);
        (void)n;
    } else {
        // The first one is still unserved, because shutting down hangs, or the watcher
        // has not run yet. Do what the signal does by default, so Ctrl-C twice always
        // works.
        signal(sig, SIG_DFL);
        raise(sig);
    }
    errno = saved;
}

// Gives the signals back to what handled them before ArmSignals().
static void DisarmSignals() {
    std::lock_guard<std::mutex> lock(SignalMutex());
    if (!signals_armed) {
        return;
    }
    signals_armed = false;
    for (size_t i = 0; i < kSignalCount; ++i) {
        sigaction(kSignals[i], &saved_actions[i], nullptr);
    }
}

static void *SignalWatcher(void *) {
    for (;;) {
        unsigned char b = 0;
        ssize_t n = read(signal_pipe[0], &b, 1);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n != 1) {
            coco_error("signal pipe read failed. n=%d errno=%d", (int)n, errno);
            return nullptr;
        }
        // From now on the signals mean what they did before, so one more ends the process
        // even if shutting down hangs.
        DisarmSignals();
        coco_trace("got signal %d, shutting down", (int)b);
        if (!shutdown_requested) {
            shutdown_signal = b;
        }
        CocoShutdown();
    }
}

static bool StartWatcher() {
    if (pipe(signal_pipe) != 0) {
        coco_error("signal pipe failed, only CocoShutdown() requests a shutdown. errno=%d", errno);
        return false;
    }
    CloseOnExec(signal_pipe[0]);
    CloseOnExec(signal_pipe[1]);
    SetNonBlocking(signal_pipe[1]);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    int err = pthread_create(&tid, &attr, SignalWatcher, nullptr);
    pthread_attr_destroy(&attr);
    if (err == 0) {
        return true;
    }
    coco_error("signal watcher failed, only CocoShutdown() requests a shutdown. err=%d", err);
    close(signal_pipe[0]);
    close(signal_pipe[1]);
    signal_pipe[0] = signal_pipe[1] = -1;
    return false;
}

// Makes SIGINT and SIGTERM request a shutdown. A signal that is ignored stays ignored, as
// for a background job that has to survive the Ctrl-C of its terminal.
static void ArmSignals() {
    std::lock_guard<std::mutex> lock(SignalMutex());
    if (signals_armed) {
        return;
    }
    if (!watcher_tried) {
        watcher_tried = true;
        watcher_ok = StartWatcher();
    }
    if (!watcher_ok) {
        return;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = OnSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    signals_seen = 0;
    for (size_t i = 0; i < kSignalCount; ++i) {
        if (sigaction(kSignals[i], nullptr, &saved_actions[i]) != 0) {
            memset(&saved_actions[i], 0, sizeof(saved_actions[i]));
            saved_actions[i].sa_handler = SIG_DFL;
        }
        if (saved_actions[i].sa_handler != SIG_IGN) {
            sigaction(kSignals[i], &sa, nullptr);
        }
    }
    signals_armed = true;
}

void WaitForShutdownOr(const std::function<bool()> &done) {
    if (CocoInit() != COCO_SUCCESS) {
        return;
    }
    // Once a shutdown has been requested the signals mean what they did before, which is
    // what lets a second Ctrl-C end a program that hangs while shutting down.
    if (!shutdown_requested) {
        ArmSignals();
    }
    ThreadShutdown &t = Local();
    if (t.waiters == nullptr) {
        t.waiters = st_cond_new();
    }
    while (!shutdown_requested && !done()) {
        // Fails only when this coroutine is interrupted, which ends the wait as well.
        if (st_cond_wait(t.waiters) != 0) {
            return;
        }
    }
}

void WaitUntilNotified(const std::function<bool()> &done) {
    if (CocoInit() != COCO_SUCCESS) {
        return;
    }
    ThreadShutdown &t = Local();
    if (t.waiters == nullptr) {
        t.waiters = st_cond_new();
    }
    // An interrupt only ends one wait, like in ConnManager::Shutdown(): what is waited for
    // has to happen first.
    while (!done()) {
        st_cond_wait(t.waiters);
    }
}

void NotifyShutdownWaiters() {
    ThreadShutdown &t = Local();
    if (t.waiters != nullptr) {
        st_cond_broadcast(t.waiters);
    }
}

void ResetShutdown() {
    shutdown_requested = false;
    shutdown_signal = 0;
}

bool RunBodyShouldStop() {
    ThreadShutdown &t = Local();
    return t.run_thread != nullptr && t.run_stop && st_thread_self() == t.run_thread;
}

void SetShutdownHook(std::function<void()> fn) { Local().hook = std::move(fn); }

int CocoWaitForShutdown() {
    WaitForShutdownOr([] { return false; });
    return shutdown_signal;
}

void CocoShutdown() {
    bool first = !shutdown_requested.exchange(true);
    // This thread is served at once; the others when their notifier runs.
    if (CocoRuntimeReady()) {
        if (first) {
            ServeShutdownHere();
        } else {
            NotifyShutdownWaiters();
        }
    }
    if (first) {
        WakeOtherThreads();
    }
}

bool CocoShutdownRequested() { return shutdown_requested; }

int CocoRun(const std::function<int()> &fn) {
    int ret = CocoInit();
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    if (!fn) {
        return ERROR_SYSTEM_ASSERT_FAILED;
    }
    ThreadShutdown &t = Local();
    if (t.run_thread != nullptr) {
        coco_error("CocoRun is already running");
        return ERROR_THREAD_STARTED;
    }
    if (!shutdown_requested) {
        ArmSignals();
    }

    struct Running {
        explicit Running(ThreadShutdown &t) : t(t) {
            t.run_thread = st_thread_self();
            t.run_stop = shutdown_requested;
            // Asked to stop before it started: its first blocking call fails, as it would
            // have had the request come while it was blocked.
            if (t.run_stop) {
                st_thread_interrupt(t.run_thread);
            }
        }
        ~Running() {
            bool stopped = t.run_stop;
            t.run_thread = nullptr;
            t.run_stop = false;
            // An interrupt the function never blocked after would fail the caller's next
            // blocking call instead; this one takes it.
            if (stopped) {
                st_usleep(0);
            }
        }
        ThreadShutdown &t;
    } running(t);
    return fn();
}

void CocoLoopMs(uint64_t) { CocoWaitForShutdown(); }

}  // namespace coco
