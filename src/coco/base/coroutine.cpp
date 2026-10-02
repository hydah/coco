#include "coco/base/coroutine.hpp"

#include <assert.h>
#include <errno.h>
#include <unistd.h>

#include <atomic>
#ifdef __linux__
#include <sys/epoll.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#endif

#include "st.h"

#include "coco/base/shutdown.hpp"
#include "coco/coco_api.h"
#include "coco/log/log.hpp"

namespace coco {

int CoroutineHandler::GetCoroutineState() { return coroutine->pull(); }

// Every thread that uses coco has a runtime of its own, as ST has a scheduler per thread.
// The ST thread-specific slot holding the running CoCoroutine; ST's keys are per thread.
static thread_local int _coroutine_key = -1;
// Never freed: a log line may still ask for the coroutine ID while the thread exits.
static thread_local CoroutineContext *_st_context = nullptr;
static thread_local bool initialized = false;

int CoroutineContext::generate_id() {
    // Shared by all threads, so an ID names one coroutine in the whole process's log.
    static std::atomic<int> next_id(100);

    int gid = next_id++;
    cache_[st_thread_self()] = gid;
    return gid;
}

int CoroutineContext::get_id() { return cache_[st_thread_self()]; }

int CoroutineContext::set_id(int v) {
    int &slot = cache_[st_thread_self()];
    int ov = slot;
    slot = v;
    return ov;
}

void CoroutineContext::clear_cid() { cache_.erase(st_thread_self()); }

// coroutine
CoCoroutine::~CoCoroutine() {
    // A detached coroutine is only freed by itself, after its cycle has returned.
    assert(!detached_ || !running());
    stop();
}

int32_t CoCoroutine::start() {
    int ret = COCO_SUCCESS;
    if ((ret = CocoInit()) != COCO_SUCCESS) {
        return ret;
    }
    owner_.Check();

    if (started) {
        coco_info("coroutine %s already running.", name.c_str());
        if (trd_err_ == COCO_SUCCESS) {
            trd_err_ = ERROR_THREAD_STARTED;
        }
        return ERROR_THREAD_STARTED;
    }
    if (disposed) {
        coco_info("coroutine %s disposed.", name.c_str());
        if (trd_err_ == COCO_SUCCESS) {
            trd_err_ = ERROR_THREAD_DISPOSED;
        }
        return ERROR_THREAD_DISPOSED;
    }

    if ((trd_ = st_thread_create(coroutine_fun, this, detached_ ? 0 : 1, stack_size)) == nullptr) {
        ret = ERROR_ST_CREATE_CYCLE_THREAD;
        coco_error("StCoroutine st_coroutine_create failed. ret=%d", ret);
        return ret;
    }
    started = true;

    return ret;
}

void CoCoroutine::stop() {
    owner_.Check();
    if (disposed) {
        return;
    }

    // ST refuses to join the calling thread; the coroutine only needs to see the interrupt.
    if (trd_ && trd_ == st_thread_self()) {
        interrupt();
        return;
    }

    disposed = true;

    interrupt();

    // When not started, the trd is null.
    if (trd_ && !detached_) {
        // The join fails with EINTR if the caller itself is interrupted while waiting;
        // returning early would free this object under a still running coroutine.
        while (st_thread_join(trd_, nullptr) != 0) {
            if (errno != EINTR) {
                coco_error("join coroutine %s failed. errno=%d", name.c_str(), errno);
                break;
            }
        }
    }

    // If there's no error occur from worker, try to set to terminated error.
    if (trd_err_ == COCO_SUCCESS && !cycle_done) {
        trd_err_ = ERROR_THREAD_TERMINATED;
    }

    return;
}

void CoCoroutine::interrupt() {
    owner_.Check();
    if (!started || interrupted || cycle_done) {
        return;
    }

    interrupted = true;
    trd_err_ = ERROR_THREAD_INTERRUPED;

    // Note that if another thread is stopping thread and waiting in
    // st_thread_join, the interrupt will make the st_thread_join fail.
    st_thread_interrupt(trd_);
}

int CoCoroutine::cycle() {
    if (_st_context) {
        cid_ = _st_context->generate_id();
        _st_context->set_id(cid_);
        coco_trace("coroutine %s cycle start", name.c_str());
    }

    return handler->Cycle();
}

void *CoCoroutine::coroutine_fun(void *arg) {
    auto p = static_cast<CoCoroutine *>(arg);

    if (_coroutine_key >= 0) {
        st_thread_setspecific(_coroutine_key, p);
    }

    int err = p->cycle();

    if (_st_context) {
        _st_context->clear_cid();
    }
    if (_coroutine_key >= 0) {
        st_thread_setspecific(_coroutine_key, nullptr);
    }

    if (err != COCO_SUCCESS) {
        p->trd_err_ = err;
    }
    // Set cycle done, no need to interrupt it.
    p->cycle_done = true;

    if (p->detached_) {
        // ST releases this stack only after the function returns, so the handler's
        // destructor may still run (and even yield) on it.
        delete p->handler;
    }

    return nullptr;
}

ListenRoutine::ListenRoutine() { coroutine = new CoCoroutine("listen", this); }

ListenRoutine::~ListenRoutine() {
    Stop();
    delete coroutine;
}

int ListenRoutine::Start() { return coroutine->start(); }
void ListenRoutine::Stop() { coroutine->stop(); }

ConnRoutine::ConnRoutine(ConnManager *manager) {
    assert(manager != nullptr);
    manager_ = manager;
    coroutine = new CoCoroutine("conn", this);
    coroutine->set_detached(true);
}

ConnRoutine::~ConnRoutine() {
    delete coroutine;
    // Last use of the manager: a waiting Shutdown() may free it once this conn yields.
    manager_->Remove(this);
}

int ConnRoutine::Start() {
    int ret = coroutine->start();
    if (ret == COCO_SUCCESS) {
        manager_->Push(this);
    }
    return ret;
}

void ConnRoutine::Stop() { coroutine->interrupt(); }

int ConnRoutine::Cycle() {
    coco_trace("[TRACE_ANCHOR] Connection, remote addr: %s", GetRemoteAddr().c_str());

    int ret = COCO_SUCCESS;
    id = CocoGetCoroutineID();
    ret = DoCycle();

    // if socket io error, set to closed.
    if (coco_is_client_gracefully_close(ret)) {
        ret = ERROR_SOCKET_CLOSED;
    }

    // success.
    if (ret == COCO_SUCCESS) {
        coco_trace("client finished.");
    }

    // client close peer.
    if (ret == ERROR_SOCKET_CLOSED) {
        coco_warn("client disconnect peer. ret=%d", ret);
    }

    return COCO_SUCCESS;
}

#ifdef __linux__
static bool st_epoll_is_supported(void) {
    struct epoll_event ev;

    ev.events = EPOLLIN;
    ev.data.ptr = NULL;
    /* Guaranteed to fail */
    epoll_ctl(-1, EPOLL_CTL_ADD, -1, &ev);

    return (errno != ENOSYS);
}
#elif defined(__APPLE__)
static bool st_kqueue_is_supported(void) {
    int kq = kqueue();
    if (kq == -1) {
        return false;
    }
    close(kq);
    return true;
}
#endif

bool CocoRuntimeReady() { return initialized; }

int CloseNetfd(st_netfd_t fd) {
    int osfd = st_netfd_fileno(fd);
    st_netfd_free(fd);
    return close(osfd);
}

int CocoInit() {
    int ret = COCO_SUCCESS;
    if (initialized) {
        return ret;
    }

#ifdef __linux__
    // check epoll, some old linux donot support epoll.
    if (!st_epoll_is_supported()) {
        ret = ERROR_ST_SET_EPOLL;
        coco_error("epoll required on Linux. ret=%d", ret);
        return ret;
    }
#elif defined(__APPLE__)
    // check kqueue, ensure kqueue is available on macOS
    if (!st_kqueue_is_supported()) {
        ret = ERROR_ST_SET_EPOLL;
        coco_error("kqueue required on macOS. ret=%d", ret);
        return ret;
    }
#endif

    // Select the best event system available on the OS. In Linux this is
    // epoll(). On BSD it will be kqueue. It is already set when an earlier call failed
    // further down.
    if (st_get_eventsys() == -1 && st_set_eventsys(ST_EVENTSYS_ALT) == -1) {
        ret = ERROR_ST_SET_EPOLL;
        coco_error("st_set_eventsys use %s failed. ret=%d", st_get_eventsys_name(), ret);
        return ret;
    }
    coco_trace("st_set_eventsys to %s", st_get_eventsys_name());

    if (st_init() != 0) {
        ret = ERROR_ST_INITIALIZE;
        coco_error("st_init failed. ret=%d", ret);
        return ret;
    }

    if (_coroutine_key < 0 && st_key_create(&_coroutine_key, NULL) != 0) {
        _coroutine_key = -1;
        ret = ERROR_ST_INITIALIZE;
        coco_error("st_key_create failed. ret=%d", ret);
        return ret;
    }

    if (!_st_context) {
        _st_context = new CoroutineContext();
        auto cid_ = _st_context->generate_id();
        _st_context->set_id(cid_);
        coco_trace("set main routine id: %d", cid_);
    }

    if ((ret = StartThreadShutdown()) != COCO_SUCCESS) {
        return ret;
    }
    coco_trace("st_init success, use %s", st_get_eventsys_name());
    initialized = true;
    return ret;
}

void CocoSleepMs(uint64_t durms) {
    if (CocoInit() == COCO_SUCCESS) {
        st_usleep(durms * 1000);
    }
}
void CocoSleep(uint32_t durs) { CocoSleepMs(uint64_t(durs) * 1000); }
int CocoGetCoroutineID() { return _st_context ? _st_context->get_id() : 0; }

bool CocoShouldStop() {
    if (_coroutine_key < 0) {
        return false;
    }
    auto c = static_cast<CoCoroutine *>(st_thread_getspecific(_coroutine_key));
    if (c != nullptr) {
        return c->pull() != COCO_SUCCESS;
    }
    // Not a coroutine coco made. The one CocoRun() runs its function on stops with a shutdown.
    return RunBodyShouldStop();
}

}  // namespace coco
