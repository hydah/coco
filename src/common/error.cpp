#include "common/error.hpp"

bool coco_is_client_gracefully_close(int error_code) {
    return error_code == ERROR_SOCKET_READ || error_code == ERROR_SOCKET_READ_FULLY ||
           error_code == ERROR_SOCKET_WRITE || error_code == ERROR_SOCKET_TIMEOUT;
}
