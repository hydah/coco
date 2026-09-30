#include "net/coco_socket.hpp"

#include <assert.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>

#include "common/error.hpp"
#include "log/log.hpp"

#define SERVER_LISTEN_BACKLOG 512

CocoSocket::CocoSocket(st_netfd_t stfd) : stfd(stfd) {
    send_timeout = recv_timeout = ST_UTIME_NO_TIMEOUT;
    recv_bytes = send_bytes = 0;
}

CocoSocket::~CocoSocket() {
    if (stfd) {
        // we must ensure the close is ok.
        int r0 = st_netfd_close(stfd);
        assert(r0 != -1);
        (void)r0;
        stfd = nullptr;
    }
}

bool CocoSocket::is_never_timeout(int64_t timeout_us) {
    return timeout_us == (int64_t)ST_UTIME_NO_TIMEOUT;
}

void CocoSocket::set_recv_timeout(int64_t timeout_us) { recv_timeout = timeout_us; }

int64_t CocoSocket::get_recv_timeout() { return recv_timeout; }

void CocoSocket::set_send_timeout(int64_t timeout_us) { send_timeout = timeout_us; }

int64_t CocoSocket::get_send_timeout() { return send_timeout; }

int64_t CocoSocket::get_recv_bytes() { return recv_bytes; }

int64_t CocoSocket::get_send_bytes() { return send_bytes; }

int CocoSocket::get_osfd() { return st_netfd_fileno(stfd); }

int CocoSocket::Read(void *buf, size_t size, ssize_t *nread) {
    int ret = COCO_SUCCESS;

    ssize_t nb_read = st_read(stfd, buf, size, recv_timeout);
    if (nread) {
        *nread = nb_read;
    }

    // On success a non-negative integer indicating the number of bytes actually
    // read is returned (a value of 0 means the network connection is closed or
    // end of file is reached). Otherwise, a value of -1 is returned and errno is
    // set to indicate the error.
    if (nb_read <= 0) {
        if (nb_read < 0 && errno == ETIME) {
            return ERROR_SOCKET_TIMEOUT;
        }

        if (nb_read == 0) {
            errno = ECONNRESET;
        }

        return ERROR_SOCKET_READ;
    }

    recv_bytes += nb_read;

    return ret;
}

int CocoSocket::ReadFully(void *buf, size_t size, ssize_t *nread) {
    int ret = COCO_SUCCESS;

    ssize_t nb_read = st_read_fully(stfd, buf, size, recv_timeout);
    if (nread) {
        *nread = nb_read;
    }

    // On success a non-negative integer indicating the number of bytes actually
    // read is returned (a value less than nbyte means the network connection is
    // closed or end of file is reached) Otherwise, a value of -1 is returned and
    // errno is set to indicate the error.
    if (nb_read != (ssize_t)size) {
        if (nb_read < 0 && errno == ETIME) {
            return ERROR_SOCKET_TIMEOUT;
        }

        if (nb_read >= 0) {
            errno = ECONNRESET;
        }

        return ERROR_SOCKET_READ_FULLY;
    }

    recv_bytes += nb_read;

    return ret;
}

int CocoSocket::Write(void *buf, size_t size, ssize_t *nwrite) {
    int ret = COCO_SUCCESS;

    ssize_t nb_write = st_write(stfd, buf, size, send_timeout);
    if (nwrite) {
        *nwrite = nb_write;
    }

    // On success a non-negative integer equal to nbyte is returned.
    // Otherwise, a value of -1 is returned and errno is set to indicate the
    // error.
    if (nb_write <= 0) {
        if (nb_write < 0 && errno == ETIME) {
            return ERROR_SOCKET_TIMEOUT;
        }

        return ERROR_SOCKET_WRITE;
    }

    send_bytes += nb_write;

    return ret;
}

int CocoSocket::Writev(const iovec *iov, int iov_size, ssize_t *nwrite) {
    int ret = COCO_SUCCESS;

    ssize_t nb_write = st_writev(stfd, iov, iov_size, send_timeout);
    if (nwrite) {
        *nwrite = nb_write;
    }

    // On success a non-negative integer equal to nbyte is returned.
    // Otherwise, a value of -1 is returned and errno is set to indicate the
    // error.
    if (nb_write <= 0) {
        if (nb_write < 0 && errno == ETIME) {
            return ERROR_SOCKET_TIMEOUT;
        }

        return ERROR_SOCKET_WRITE;
    }

    send_bytes += nb_write;

    return ret;
}

int CocoSocket::recvfrom(void *buf, int size, ssize_t *nread, struct sockaddr *from, int *fromlen) {
    int ret = COCO_SUCCESS;

    ssize_t nb_read = st_recvfrom(stfd, buf, size, from, fromlen, recv_timeout);
    if (nread) {
        *nread = nb_read;
    }

    // On success a non-negative integer indicating the number of bytes actually
    // read is returned (a value of 0 means the network connection is closed or
    // end of file is reached). Otherwise, a value of -1 is returned and errno is
    // set to indicate the error.
    if (nb_read <= 0) {
        if (nb_read < 0 && errno == ETIME) {
            return ERROR_SOCKET_TIMEOUT;
        }

        if (nb_read == 0) {
            errno = ECONNRESET;
        }

        return ERROR_SOCKET_READ;
    }

    recv_bytes += nb_read;

    return ret;
}

int CocoSocket::sendto(void *buf, int size, ssize_t *nwrite, struct sockaddr *to, int tolen) {
    int ret = COCO_SUCCESS;

    ssize_t nb_write = st_sendto(stfd, buf, size, to, tolen, send_timeout);
    if (nwrite) {
        *nwrite = nb_write;
    }

    // On success a non-negative integer equal to nbyte is returned.
    // Otherwise, a value of -1 is returned and errno is set to indicate the
    // error.
    if (nb_write <= 0) {
        if (nb_write < 0 && errno == ETIME) {
            return ERROR_SOCKET_TIMEOUT;
        }

        return ERROR_SOCKET_WRITE;
    }

    send_bytes += nb_write;

    return ret;
}

int CocoSocket::recvmsg(ssize_t *nread, struct msghdr *msg, int flags) {
    int ret = COCO_SUCCESS;

    ssize_t nb_read = st_recvmsg(stfd, msg, flags, recv_timeout);
    if (nread) {
        *nread = nb_read;
    }

    // On success a non-negative integer indicating the number of bytes actually
    // read is returned (a value of 0 means the network connection is closed or
    // end of file is reached). Otherwise, a value of -1 is returned and errno is
    // set to indicate the error.
    if (nb_read <= 0) {
        if (nb_read < 0 && errno == ETIME) {
            return ERROR_SOCKET_TIMEOUT;
        }

        if (nb_read == 0) {
            errno = ECONNRESET;
        }

        return ERROR_SOCKET_READ;
    }

    recv_bytes += nb_read;

    return ret;
}

int CocoSocket::sendmsg(ssize_t *nwrite, struct msghdr *msg, int flags) {
    int ret = COCO_SUCCESS;

    ssize_t nb_write = st_sendmsg(stfd, msg, flags, send_timeout);
    if (nwrite) {
        *nwrite = nb_write;
    }

    // On success a non-negative integer equal to nbyte is returned.
    // Otherwise, a value of -1 is returned and errno is set to indicate the
    // error.
    if (nb_write <= 0) {
        if (nb_write < 0 && errno == ETIME) {
            return ERROR_SOCKET_TIMEOUT;
        }

        return ERROR_SOCKET_WRITE;
    }

    send_bytes += nb_write;

    return ret;
}

namespace {

class AddrInfo {
 public:
    AddrInfo() = default;
    ~AddrInfo() {
        if (p) {
            freeaddrinfo(p);
        }
    }
    AddrInfo(const AddrInfo &) = delete;
    AddrInfo &operator=(const AddrInfo &) = delete;

    addrinfo *p = nullptr;
};

// Closes the fd unless Release() handed it on.
class FdGuard {
 public:
    explicit FdGuard(int fd) : fd_(fd) {}
    ~FdGuard() {
        if (fd_ != -1) {
            ::close(fd_);
        }
    }
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;

    int get() const { return fd_; }
    void Release() { fd_ = -1; }

 private:
    int fd_;
};

int Resolve(const std::string &host, int port, int family, int socktype, int flags,
            AddrInfo *out) {
    char port_string[8];
    snprintf(port_string, sizeof(port_string), "%d", port);
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    hints.ai_flags = flags;
    int r0 = getaddrinfo(host.c_str(), port_string, &hints, &out->p);
    if (r0 != 0) {
        coco_error("resolve %s:%d failed: %s", host.c_str(), port, gai_strerror(r0));
        return ERROR_SYSTEM_IP_INVALID;
    }
    return COCO_SUCCESS;
}

// On failure the fd stays with the guard.
int OpenSt(FdGuard *fd, st_netfd_t *stfd) {
    st_netfd_t s = st_netfd_open_socket(fd->get());
    if (s == nullptr) {
        coco_error("st_netfd_open_socket failed. fd=%d, errno=%d", fd->get(), errno);
        return ERROR_ST_OPEN_SOCKET;
    }
    fd->Release();
    *stfd = s;
    return COCO_SUCCESS;
}

}  // namespace

int ListenSocket(const std::string &ip, int port, int socktype, st_netfd_t *stfd) {
    int ret = COCO_SUCCESS;
    const char *ep = ip.c_str();

    AddrInfo ai;
    int family = is_ipv6(ip) ? AF_INET6 : AF_INET;
    if ((ret = Resolve(ip, port, family, socktype, AI_NUMERICHOST | AI_PASSIVE, &ai)) !=
        COCO_SUCCESS) {
        return ret;
    }
    const addrinfo *r = ai.p;

    FdGuard fd(socket(r->ai_family, r->ai_socktype, r->ai_protocol));
    if (fd.get() == -1) {
        coco_error("create socket error. ep=%s:%d, errno=%d", ep, port, errno);
        return ERROR_SOCKET_CREATE;
    }

    if (socktype == SOCK_STREAM) {
        int reuse = 1;
        if (setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
            coco_error("setsockopt reuse-addr error. ep=%s:%d, errno=%d", ep, port, errno);
            return ERROR_SOCKET_SETREUSE;
        }
    }

    if (bind(fd.get(), r->ai_addr, r->ai_addrlen) == -1) {
        coco_error("bind socket error. ep=%s:%d, errno=%d", ep, port, errno);
        return ERROR_SOCKET_BIND;
    }

    if (socktype == SOCK_STREAM && ::listen(fd.get(), SERVER_LISTEN_BACKLOG) == -1) {
        coco_error("listen socket error. ep=%s:%d, errno=%d", ep, port, errno);
        return ERROR_SOCKET_LISTEN;
    }

    int osfd = fd.get();
    if ((ret = OpenSt(&fd, stfd)) != COCO_SUCCESS) {
        return ret;
    }
    coco_dbg("listen success. ep=%s:%d, fd=%d", ep, port, osfd);
    return ret;
}

int DialStream(const std::string &host, int port, int64_t timeout_us, st_netfd_t *stfd) {
    int ret = COCO_SUCCESS;

    AddrInfo ai;
    if ((ret = Resolve(host, port, AF_UNSPEC, SOCK_STREAM, 0, &ai)) != COCO_SUCCESS) {
        return ret;
    }

    ret = ERROR_ST_CONNECT;
    for (const addrinfo *r = ai.p; r != nullptr; r = r->ai_next) {
        FdGuard fd(socket(r->ai_family, r->ai_socktype, r->ai_protocol));
        if (fd.get() == -1) {
            coco_error("create socket error. errno=%d", errno);
            ret = ERROR_SOCKET_CREATE;
            continue;
        }

        st_netfd_t s = nullptr;
        if ((ret = OpenSt(&fd, &s)) != COCO_SUCCESS) {
            continue;
        }

        if (st_connect(s, r->ai_addr, r->ai_addrlen, (st_utime_t)timeout_us) == 0) {
            *stfd = s;
            coco_info("connect ok. server=%s, port=%d", host.c_str(), port);
            return COCO_SUCCESS;
        }
        bool interrupted = errno == EINTR;
        st_netfd_close(s);
        ret = ERROR_ST_CONNECT;
        if (interrupted) {
            break;
        }
    }

    coco_error("connect to server error. server=%s, port=%d, ret=%d", host.c_str(), port, ret);
    return ret;
}

int DialDatagram(const std::string &host, int port, st_netfd_t *stfd, sockaddr_storage *peer,
                 socklen_t *peer_len) {
    int ret = COCO_SUCCESS;

    AddrInfo ai;
    if ((ret = Resolve(host, port, AF_UNSPEC, SOCK_DGRAM, 0, &ai)) != COCO_SUCCESS) {
        return ret;
    }
    const addrinfo *r = ai.p;

    FdGuard fd(socket(r->ai_family, r->ai_socktype, r->ai_protocol));
    if (fd.get() == -1) {
        coco_error("create socket error. errno=%d", errno);
        return ERROR_SOCKET_CREATE;
    }
    if ((ret = OpenSt(&fd, stfd)) != COCO_SUCCESS) {
        return ret;
    }

    assert(r->ai_addrlen <= sizeof(*peer));
    memcpy(peer, r->ai_addr, r->ai_addrlen);
    *peer_len = r->ai_addrlen;
    return ret;
}