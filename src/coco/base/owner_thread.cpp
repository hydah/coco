#include "coco/base/owner_thread.hpp"

#include <assert.h>

namespace coco {

void OwnerThread::Check() const {
    assert(id_ == std::this_thread::get_id() && "coco object used from another thread");
}

}  // namespace coco
