#pragma once

#include "st/obj/st.h"

#include "base/coroutine.hpp"
#include "utils/utils.hpp"

class CocoSocket : public IoReaderWriter {
 public:
    CocoSocket(st_netfd_t client_stfd);
    virtual ~CocoSocket() = default;

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
    int64_t recv_timeout;
    int64_t send_timeout;
    int64_t recv_bytes;
    int64_t send_bytes;
    st_netfd_t stfd;
};