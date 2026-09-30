#include "base/coroutine_mgr.hpp"
#include "base/coroutine.hpp"

#include "common/error.hpp"
#include "log/log.hpp"

#include <algorithm>

ConnManager::~ConnManager() {
    if (cleanup_trd_) {
        quit_ = true;
        st_thread_interrupt(cleanup_trd_);
        st_thread_join(cleanup_trd_, NULL);
        cleanup_trd_ = nullptr;
    }

    Destroy();

    for (auto conn : conns) {
        if (conn != nullptr) {
            delete conn;
        }
    }
    conns.clear();

    if (cond_) {
        st_cond_destroy(cond_);
        cond_ = nullptr;
    }
}

void ConnManager::Push(ConnRoutine *conn) {
    if (std::find(conns.begin(), conns.end(), conn) == conns.end()) {
        conns.push_back(conn);
    }
}

void ConnManager::Remove(ConnRoutine *conn) {
    auto it = std::find(conns.begin(), conns.end(), conn);

    // removed by destroy, ignore.
    if (it == conns.end()) {
        coco_warn("server moved connection, ignore.");
        return;
    }

    conns.erase(it);
    coco_info("conn removed. conns=%d", (int)conns.size());
    zombies.push_back(conn);

    if (!cleanup_trd_) {
        cond_ = st_cond_new();
        cleanup_trd_ = st_thread_create(CleanupLoop, this, 1, 0);
        if (!cleanup_trd_) {
            coco_error("create conn cleanup coroutine failed");
            return;
        }
    }
    st_cond_signal(cond_);
}

void ConnManager::Destroy() {
    // Deleting a conn joins its coroutine and may yield, so detach the list
    // first to keep a concurrent Destroy() from freeing the same conn twice.
    std::vector<ConnRoutine *> dead;
    dead.swap(zombies);

    for (auto conn : dead) {
        if (conn) {
            delete conn;
        }
    }
}

void *ConnManager::CleanupLoop(void *arg) {
    ConnManager *mgr = (ConnManager *)arg;
    while (!mgr->quit_) {
        // A signal sent while we were busy in Destroy() is lost, so only wait
        // when there is really nothing to clean up.
        if (mgr->zombies.empty()) {
            st_cond_wait(mgr->cond_);
        }
        if (mgr->quit_) {
            break;
        }
        mgr->Destroy();
    }
    return NULL;
}
