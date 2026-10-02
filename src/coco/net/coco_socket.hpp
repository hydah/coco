#pragma once

#include <sys/socket.h>

#include <string>

#include "coco/base/st_fwd.hpp"

#include "coco/base/coroutine.hpp"
#include "coco/utils/io.hpp"

namespace coco {

// Owns an st_netfd_t: the fd is closed when the socket is destroyed. No coroutine may
// still be blocked on it by then. It belongs to the runtime of the thread that created it.
class CocoSocket : public IoReaderWriter {
 public:
    explicit CocoSocket(st_netfd_t stfd);
    virtual ~CocoSocket();

    CocoSocket(const CocoSocket &) = delete;
    CocoSocket &operator=(const CocoSocket &) = delete;

    st_netfd_t get_stfd() { return stfd; }
    // Gives up the fd without closing it and returns it, or -1 if there is none. Every
    // call after it fails. No coroutine may be blocked on the socket.
    int release();
    virtual bool is_never_timeout(int64_t timeout_us);
    virtual void set_recv_timeout(int64_t timeout_us);
    virtual int64_t get_recv_timeout();
    virtual void set_send_timeout(int64_t timeout_us);
    virtual int64_t get_send_timeout();
    virtual int64_t get_recv_bytes();
    virtual int64_t get_send_bytes();
    virtual int get_osfd();

    virtual int Read(void *buf, size_t size, ssize_t *nread);
    virtual int ReadFully(void *buf, size_t size, ssize_t *nread);
    virtual int Write(void *buf, size_t size, ssize_t *nwrite);
    virtual int Writev(const iovec *iov, int iov_size, ssize_t *nwrite);

    virtual int recvfrom(void *buf, int size, ssize_t *nread, struct sockaddr *from, int *fromlen);
    virtual int sendto(void *buf, int size, ssize_t *nwrite, struct sockaddr *to, int tolen);
    virtual int recvmsg(ssize_t *nread, struct msghdr *msg, int flags);
    virtual int sendmsg(ssize_t *nwrite, struct msghdr *msg, int flags);

 private:
    // ERROR_SOCKET_CLOSED once the fd was released.
    int check();

    int64_t recv_timeout;
    int64_t send_timeout;
    int64_t recv_bytes;
    int64_t send_bytes;
    st_netfd_t stfd;
    OwnerThread owner_;
};

// Binds a socket of socktype (SOCK_STREAM or SOCK_DGRAM) to ip:port, which must be an IP
// literal; a stream socket also listens. On success *stfd owns the fd.
int ListenSocket(const std::string &ip, int port, int socktype, st_netfd_t *stfd);

// Resolves host and connects a stream socket to the first address that accepts within
// timeout_us per attempt. On success *stfd owns the fd.
int DialStream(const std::string &host, int port, int64_t timeout_us, st_netfd_t *stfd);

// Resolves host and opens an unconnected datagram socket of the matching family; *peer
// gets the address to send to. On success *stfd owns the fd.
int DialDatagram(const std::string &host, int port, st_netfd_t *stfd, sockaddr_storage *peer,
                 socklen_t *peer_len);

}  // namespace coco
