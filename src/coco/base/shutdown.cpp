#include "coco/base/shutdown.hpp"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "st.h"

#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/log/log.hpp"

namespace coco {

static bool shutdown_requested = false;
// The signal that asked for it, 0 for CocoShutdown().
static int shutdown_signal = 0;
// Broadcast whenever a waiter's condition may have changed.
static st_cond_t waiters = nullptr;

// The coroutine CocoRun() runs its function on, and whether a shutdown was requested while
// it does.
static st_thread_t run_thread = nullptr;
static bool run_stop = false;

// SIGINT and SIGTERM request a shutdown. The handler only writes the signal number to a
// pipe; SignalWatcher reads it on a coroutine and calls CocoShutdown(), so no coroutine
// code runs in a signal handler.
static const int kSignals[] = {SIGINT, SIGTERM};
static const size_t kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);
static struct sigaction saved_actions[kSignalCount];
static int signal_pipe[2] = {-1, -1};
static bool watcher_tried = false;
static bool watcher_ok = false;
static bool signals_armed = false;
// Signals the handler has taken since it was armed.
static volatile sig_atomic_t signals_seen = 0;

static void OnSignal(int sig) {
    int saved = errno;
    if (signals_seen++ == 0) {
        unsigned char b = (unsigned char)sig;
        // The pipe is non-blocking; a full one only means signals are pending anyway.
        ssize_t n = write(signal_pipe[1], &b, 1);
        (void)n;
    } else {
        // The first one is still unserved, because no coroutine gets to run, or shutting
        // down hangs. Do what the signal does by default, so Ctrl-C twice always works.
        signal(sig, SIG_DFL);
        raise(sig);
    }
    errno = saved;
}

// Gives the signals back to what handled them before ArmSignals().
static void DisarmSignals() {
    if (!signals_armed) {
        return;
    }
    signals_armed = false;
    for (size_t i = 0; i < kSignalCount; ++i) {
        sigaction(kSignals[i], &saved_actions[i], nullptr);
    }
}

static void *SignalWatcher(void *arg) {
    st_netfd_t rfd = static_cast<st_netfd_t>(arg);
    for (;;) {
        unsigned char b = 0;
        ssize_t n = st_read(rfd, &b, 1, ST_UTIME_NO_TIMEOUT);
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
    for (int i = 0; i < 2; ++i) {
        fcntl(signal_pipe[i], F_SETFD, FD_CLOEXEC);
    }
    fcntl(signal_pipe[1], F_SETFL, fcntl(signal_pipe[1], F_GETFL) | O_NONBLOCK);

    st_netfd_t rfd = st_netfd_open(signal_pipe[0]);
    if (rfd != nullptr && st_thread_create(SignalWatcher, rfd, 0, 0) != nullptr) {
        return true;
    }
    coco_error("signal watcher failed, only CocoShutdown() requests a shutdown. errno=%d", errno);
    if (rfd != nullptr) {
        st_netfd_close(rfd);
    } else {
        close(signal_pipe[0]);
    }
    close(signal_pipe[1]);
    signal_pipe[0] = signal_pipe[1] = -1;
    return false;
}

// Makes SIGINT and SIGTERM request a shutdown. A signal that is ignored stays ignored, as
// for a background job that has to survive the Ctrl-C of its terminal.
static void ArmSignals() {
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
    if (waiters == nullptr) {
        waiters = st_cond_new();
    }
    while (!shutdown_requested && !done()) {
        // Fails only when this coroutine is interrupted, which ends the wait as well.
        if (st_cond_wait(waiters) != 0) {
            return;
        }
    }
}

void WaitUntilNotified(const std::function<bool()> &done) {
    if (CocoInit() != COCO_SUCCESS) {
        return;
    }
    if (waiters == nullptr) {
        waiters = st_cond_new();
    }
    // An interrupt only ends one wait, like in ConnManager::Shutdown(): what is waited for
    // has to happen first.
    while (!done()) {
        st_cond_wait(waiters);
    }
}

void NotifyShutdownWaiters() {
    if (waiters != nullptr) {
        st_cond_broadcast(waiters);
    }
}

void ResetShutdown() {
    shutdown_requested = false;
    shutdown_signal = 0;
}

bool RunBodyShouldStop() {
    return run_thread != nullptr && run_stop && st_thread_self() == run_thread;
}

int CocoWaitForShutdown() {
    WaitForShutdownOr([] { return false; });
    return shutdown_signal;
}

void CocoShutdown() {
    bool first = !shutdown_requested;
    shutdown_requested = true;
    if (first && run_thread != nullptr) {
        run_stop = true;
        // Ends the blocking call CocoRun()'s function is in. The caller itself is running,
        // not blocked, so there is nothing to end.
        if (run_thread != st_thread_self()) {
            st_thread_interrupt(run_thread);
        }
    }
    NotifyShutdownWaiters();
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
    if (run_thread != nullptr) {
        coco_error("CocoRun is already running");
        return ERROR_THREAD_STARTED;
    }
    if (!shutdown_requested) {
        ArmSignals();
    }

    struct Running {
        Running() {
            run_thread = st_thread_self();
            run_stop = shutdown_requested;
            // Asked to stop before it started: its first blocking call fails, as it would
            // have had the request come while it was blocked.
            if (run_stop) {
                st_thread_interrupt(run_thread);
            }
        }
        ~Running() {
            bool stopped = run_stop;
            run_thread = nullptr;
            run_stop = false;
            // An interrupt the function never blocked after would fail the caller's next
            // blocking call instead; this one takes it.
            if (stopped) {
                st_usleep(0);
            }
        }
    } running;
    return fn();
}

void CocoLoopMs(uint64_t) { CocoWaitForShutdown(); }

}  // namespace coco
