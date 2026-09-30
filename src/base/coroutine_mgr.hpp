#pragma once
#include <vector>

#include "st.h"

class ConnRoutine;

// Owns connection routines. A finished connection cannot free itself (it is
// still running on its own stack), so Remove() hands it to a cleanup coroutine
// which deletes it, and thereby closes its socket, right after it exits.
class ConnManager {
 public:
    ConnManager() = default;
    virtual ~ConnManager();

    virtual void Push(ConnRoutine *conn);
    virtual void Remove(ConnRoutine *conn);
    virtual void Destroy();

 private:
    static void *CleanupLoop(void *arg);

    std::vector<ConnRoutine *> conns;
    std::vector<ConnRoutine *> zombies;

    // Created lazily in Remove(), which always runs inside a coroutine.
    st_cond_t cond_ = nullptr;
    st_thread_t cleanup_trd_ = nullptr;
    bool quit_ = false;
};
