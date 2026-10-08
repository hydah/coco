#include "coco/common/error.hpp"

namespace coco {

bool coco_is_client_gracefully_close(int error_code) {
    return error_code == ERROR_SOCKET_READ || error_code == ERROR_SOCKET_READ_FULLY ||
           error_code == ERROR_SOCKET_WRITE || error_code == ERROR_SOCKET_TIMEOUT ||
           error_code == ERROR_RUDP_RESET;
}

}  // namespace coco
