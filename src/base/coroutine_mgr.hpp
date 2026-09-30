#pragma once
#include <stddef.h>

#include <unordered_set>

#include "st.h"

class ConnRoutine;

// Tracks the running connection routines. It never frees them: a connection deletes
// itself when its coroutine exits and unregisters from its destructor. The manager
// must outlive every connection registered with it.
class ConnManager {
 public:
    ConnManager() = default;
    // Shuts down the remaining connections first.
    virtual ~ConnManager();

    ConnManager(const ConnManager &) = delete;
    ConnManager &operator=(const ConnManager &) = delete;

    virtual void Push(ConnRoutine *conn);
    virtual void Remove(ConnRoutine *conn);
    // Interrupts every connection and waits until all of them have exited. Must not be
    // called from one of those connections, which would wait for itself.
    virtual void Shutdown();
    size_t Size() const { return conns.size(); }

 private:
    std::unordered_set<ConnRoutine *> conns;
    // Created by the first Shutdown() that has to wait; signalled when conns drains.
    st_cond_t cond_ = nullptr;
};
