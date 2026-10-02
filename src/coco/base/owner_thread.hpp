#pragma once

#include <thread>

namespace coco {

// The thread that created an object. Coroutines and sockets belong to the runtime of the
// thread that made them; in a build without NDEBUG, Check() aborts when another thread
// uses one. A release build checks nothing.
class OwnerThread {
 public:
    OwnerThread() : id_(std::this_thread::get_id()) {}

    void Check() const;

 private:
    std::thread::id id_;
};

}  // namespace coco
