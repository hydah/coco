#include "coco/net/layer4/coco_layer4.hpp"

#include "coco/common/error.hpp"
#include "coco/utils/utils.hpp"

namespace coco {

std::string FormatSockaddr(const struct sockaddr *addr, socklen_t addrlen) {
    return FormatAddr(addr, addrlen);
}

int StreamConn::ReadFully(void *buf, size_t size, ssize_t *nread) {
    int err = COCO_SUCCESS;
    size_t got = 0;
    while (got < size) {
        ssize_t n = 0;
        if ((err = Read((char *)buf + got, size - got, &n)) != COCO_SUCCESS) {
            break;
        }
        got += (size_t)n;
    }
    if (nread) {
        *nread = (ssize_t)got;
    }
    return err;
}

}  // namespace coco
