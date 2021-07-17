#pragma once

#include "st/obj/st.h"

#include "base/coroutine.hpp"
#include "utils/utils.hpp"

class CocoSocket : public IBufferReader, public IBufferWriter {

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

  /**
   * @param nread, the actual read bytes, ignore if NULL.
   */
  virtual int read(void *buf, size_t size, ssize_t *nread);
  virtual int read_fully(void *buf, size_t size, ssize_t *nread);
  /**
   * @param nwrite, the actual write bytes, ignore if NULL.
   */
  virtual int write(void *buf, size_t size, ssize_t *nwrite);
  virtual int writev(const iovec *iov, int iov_size, ssize_t *nwrite);

  virtual int recvfrom(void *buf, int size, ssize_t *nread,
                       struct sockaddr *from, int *fromlen);
  virtual int sendto(void *buf, int size, ssize_t *nwrite, struct sockaddr *to,
                     int tolen);
  virtual int recvmsg(ssize_t *nread, struct msghdr *msg, int flags);
  virtual int sendmsg(ssize_t *nwrite, struct msghdr *msg, int flags);

private:
  int64_t recv_timeout;
  int64_t send_timeout;
  int64_t recv_bytes;
  int64_t send_bytes;
  st_netfd_t stfd;
};