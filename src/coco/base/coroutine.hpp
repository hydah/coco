#pragma once

#include <map>
#include <string>
#include <utility>

#include "coco/base/st_fwd.hpp"

#include "coco/base/coroutine_mgr.hpp"
#include "coco/base/owner_thread.hpp"
#include "coco/common/error.hpp"

namespace coco {

constexpr int32_t kInvalidContextId = -1;
class CoCoroutine;

// The body run by a CoCoroutine. Use ListenRoutine or ConnRoutine rather than deriving
// from this directly: they create and own the coroutine.
class CoroutineHandler {
 public:
    CoroutineHandler() = default;
    virtual ~CoroutineHandler() = default;

    CoroutineHandler(const CoroutineHandler &) = delete;
    CoroutineHandler &operator=(const CoroutineHandler &) = delete;

    // Runs on the coroutine's own stack. A non-success return becomes the coroutine's error.
    virtual int Cycle() = 0;
    // COCO_SUCCESS while the coroutine may keep running, otherwise why it has to stop.
    int GetCoroutineState();
    // Long-running Cycle() loops should check this and return once it is true.
    bool ShouldTermCycle() { return GetCoroutineState() != COCO_SUCCESS; }

 protected:
    // Must be set by the derived constructor; every other member assumes it is non-null.
    CoCoroutine *coroutine = nullptr;
};

// True once CocoInit() has succeeded on the calling thread.
bool CocoRuntimeReady();

// Closes fd and frees its netfd. Use it instead of st_netfd_close, which puts the netfd on
// the free list every thread shares and only then reads the fd to close: another thread
// may have taken the netfd for a new fd by then, which would be closed instead. Unlike
// st_netfd_close it cannot tell whether a coroutine is still blocked on fd; none may be.
int CloseNetfd(st_netfd_t fd);

// Per-coroutine ID, keyed by the ST thread. Each thread's runtime has its own.
class CoroutineContext {
 public:
    CoroutineContext() = default;
    virtual ~CoroutineContext() = default;

    // Allocates a new ID and binds it to the calling coroutine.
    virtual int generate_id();
    // Returns 0 if the calling coroutine has no ID yet.
    virtual int get_id();
    // Returns the previous ID, or 0 if there was none.
    virtual int set_id(int v);
    virtual void clear_cid();

 private:
    std::map<st_thread_t, int> cache_;
};

// Wraps one ST thread running a CoroutineHandler. Single use: once stopped it cannot be
// started again.
class CoCoroutine {
 public:
    // The handler is not owned unless the coroutine is detached. The cid is replaced by a
    // freshly generated one as soon as the coroutine starts running.
    CoCoroutine(std::string n, CoroutineHandler *h) : CoCoroutine(std::move(n), h, kInvalidContextId) {}
    CoCoroutine(std::string n, CoroutineHandler *h, int32_t cid) : name(std::move(n)), handler(h), cid_(cid) {}
    ~CoCoroutine();

    CoCoroutine(const CoCoroutine &) = delete;
    CoCoroutine &operator=(const CoCoroutine &) = delete;

    void set_stack_size(int v) { stack_size = v; }
    // A detached coroutine cannot be joined; when the handler's Cycle() returns, the
    // coroutine deletes the handler (and with it this object) on its own stack.
    void set_detached(bool v) { detached_ = v; }
    int32_t start();
    // Interrupts the coroutine and, unless it is detached or the caller is the coroutine
    // itself, waits for it to exit.
    void stop();
    // Wakes the coroutine from any blocking ST call and marks it as interrupted; never waits.
    void interrupt();
    bool running() const { return started && !cycle_done; }
    // 在handler cycle中，如果发现 coroutine err了，要退出cycle
    int32_t pull() const { return trd_err_; }
    int32_t get_cid() const { return cid_; }

 private:
    int32_t cycle();
    static void *coroutine_fun(void *arg);

    std::string name;
    // 0 uses ST's default, which is 64K.
    int stack_size = 0;
    CoroutineHandler *handler = nullptr;
    st_thread_t trd_ = nullptr;
    // Why the coroutine has to stop: set by start() failures, interrupt(), stop(), or a
    // non-success return from the handler.
    int trd_err_ = COCO_SUCCESS;
    int32_t cid_ = kInvalidContextId;

    // start() created the ST thread.
    bool started = false;
    // interrupt() already signalled the thread; it is never sent twice.
    bool interrupted = false;
    // stop() has run; start() will refuse from now on.
    bool disposed = false;
    // The handler's Cycle() has returned.
    bool cycle_done = false;
    bool detached_ = false;
    OwnerThread owner_;
};

// Owned by the caller. Derived destructors must call Stop() before freeing anything
// Cycle() uses: the base destructor runs after they are gone.
class ListenRoutine : public CoroutineHandler {
 public:
    ListenRoutine();
    ~ListenRoutine() override;

    virtual int Start();
    // Interrupts Cycle() and waits for it to return.
    virtual void Stop();
};

// Owns itself once Start() succeeds: the object is deleted by its own coroutine right
// after Cycle() returns. Other code must not delete a started connection, and must not
// keep a pointer to it past its destructor.
class ConnRoutine : public CoroutineHandler {
 public:
    explicit ConnRoutine(ConnManager *manager);
    ~ConnRoutine() override;

    // Registers with the manager on success. On failure the caller still owns the object.
    virtual int Start();
    int Cycle() override;
    // Interrupts the connection without waiting; it deletes itself when DoCycle() returns.
    virtual void Stop();
    virtual std::string GetRemoteAddr() = 0;

 protected:
    virtual int DoCycle() = 0;
    ConnManager *manager_;

 private:
    // Coroutine ID, only valid once Cycle() has started.
    int id;
};

}  // namespace coco
