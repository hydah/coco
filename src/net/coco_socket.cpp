#include "net/coco_socket.hpp"

#include <assert.h>

#include "common/error.hpp"
#include "log/log.hpp"

CocoSocket::CocoSocket(st_netfd_t client_stfd) {
  stfd = client_stfd;
  send_timeout = recv_timeout = ST_UTIME_NO_TIMEOUT;
  recv_bytes = send_bytes = 0;
}

bool CocoSocket::is_never_timeout(int64_t timeout_us) {
  return timeout_us == (int64_t)ST_UTIME_NO_TIMEOUT;
}

void CocoSocket::set_recv_timeout(int64_t timeout_us) {
  recv_timeout = timeout_us;
}

int64_t CocoSocket::get_recv_timeout() { return recv_timeout; }

void CocoSocket::set_send_timeout(int64_t timeout_us) {
  send_timeout = timeout_us;
}

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
    // @see https://github.com/ossrs/srs/issues/200
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
    // @see https://github.com/ossrs/srs/issues/200
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
    // @see https://github.com/ossrs/srs/issues/200
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
    // @see https://github.com/ossrs/srs/issues/200
    if (nb_write < 0 && errno == ETIME) {
      return ERROR_SOCKET_TIMEOUT;
    }

    return ERROR_SOCKET_WRITE;
  }

  send_bytes += nb_write;

  return ret;
}

int CocoSocket::recvfrom(void *buf, int size, ssize_t *nread,
                         struct sockaddr *from, int *fromlen) {
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
    // @see https://github.com/ossrs/srs/issues/200
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

int CocoSocket::sendto(void *buf, int size, ssize_t *nwrite,
                       struct sockaddr *to, int tolen) {
  int ret = COCO_SUCCESS;

  ssize_t nb_write = st_sendto(stfd, buf, size, to, tolen, send_timeout);
  if (nwrite) {
    *nwrite = nb_write;
  }

  // On success a non-negative integer equal to nbyte is returned.
  // Otherwise, a value of -1 is returned and errno is set to indicate the
  // error.
  if (nb_write <= 0) {
    // @see https://github.com/ossrs/srs/issues/200
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
    // @see https://github.com/ossrs/srs/issues/200
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
    // @see https://github.com/ossrs/srs/issues/200
    if (nb_write < 0 && errno == ETIME) {
      return ERROR_SOCKET_TIMEOUT;
    }

    return ERROR_SOCKET_WRITE;
  }

  send_bytes += nb_write;

  return ret;
}