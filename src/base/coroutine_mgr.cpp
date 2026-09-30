#include "base/coroutine_mgr.hpp"
#include "base/coroutine.hpp"

#include "log/log.hpp"

#include <vector>

ConnManager::~ConnManager() {
    Shutdown();

    if (cond_) {
        st_cond_destroy(cond_);
        cond_ = nullptr;
    }
}

void ConnManager::Push(ConnRoutine *conn) { conns.insert(conn); }

void ConnManager::Remove(ConnRoutine *conn) {
    if (conns.erase(conn) == 0) {
        return;
    }
    coco_info("conn removed. conns=%d", (int)conns.size());

    if (conns.empty() && cond_) {
        st_cond_broadcast(cond_);
    }
}

void ConnManager::Shutdown() {
    if (conns.empty()) {
        return;
    }
    if (!cond_) {
        cond_ = st_cond_new();
    }

    std::vector<ConnRoutine *> live(conns.begin(), conns.end());
    for (auto conn : live) {
        conn->Stop();
    }

    // The last Remove() can only run while we are parked here, so the broadcast is not lost.
    // An interrupt of this coroutine just ends one wait early.
    while (!conns.empty()) {
        st_cond_wait(cond_);
    }
}
