#ifndef SRS_ST_SOCKET_HPP
#define SRS_ST_SOCKET_HPP

#include "st/obj/st.h"
#include "coco/base/st_coroutine.hpp"
#include "coco/base/utils.hpp"

/**
 * the socket provides TCP socket over st,
 * that is, the sync socket mechanism.
 */
class StSocket : public IBufferReader, public IBufferWriter
{
private:
    int64_t recv_timeout;
    int64_t send_timeout;
    int64_t recv_bytes;
    int64_t send_bytes;
    st_netfd_t stfd;
public:
    StSocket(st_netfd_t client_stfd);
    virtual ~StSocket();
public:
    virtual bool is_never_timeout(int64_t timeout_us);
    virtual void set_recv_timeout(int64_t timeout_us);
    virtual int64_t get_recv_timeout();
    virtual void set_send_timeout(int64_t timeout_us);
    virtual int64_t get_send_timeout();
    virtual int64_t get_recv_bytes();
    virtual int64_t get_send_bytes();
    virtual int get_osfd();
public:
    /**
     * @param nread, the actual read bytes, ignore if NULL.
     */
    virtual int read(void* buf, size_t size, ssize_t* nread);
    virtual int read_fully(void* buf, size_t size, ssize_t* nread);
    /**
     * @param nwrite, the actual write bytes, ignore if NULL.
     */
    virtual int write(void* buf, size_t size, ssize_t* nwrite);
    virtual int writev(const iovec *iov, int iov_size, ssize_t* nwrite);

    virtual int recvfrom(void *buf, int size, ssize_t* nread, struct sockaddr *from, int *fromlen);
    virtual int sendto(void *buf, int size, ssize_t* nwrite, struct sockaddr *to, int tolen);
    virtual int recvmsg(ssize_t* nread, struct msghdr *msg, int flags);
    virtual int sendmsg(ssize_t* nwrite, struct msghdr *msg, int flags);
};
#endif