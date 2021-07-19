#include "base/coroutine_mgr.hpp"
#include "base/coroutine.hpp"

#include "common/error.hpp"
#include "log/log.hpp"

#include <algorithm>

ConnManager::~ConnManager() {
    for (auto conn : conns) {
        if (conn != nullptr) {
            delete conn;
            conn = nullptr;
        }
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
}

void ConnManager::Destroy() {
    for (auto conn : zombies) {
        if (conn) {
            delete conn;
        }
    }
    zombies.clear();
}