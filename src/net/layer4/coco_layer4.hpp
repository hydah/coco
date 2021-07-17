#pragma once
#include <unistd.h>

#include "net/coco_socket.hpp"
#include <assert.h>

class Layer4Conn {
public:
  Layer4Conn(st_netfd_t stfd) {
    stfd_ = stfd;
    skt_ = new CocoSocket(stfd);
  }
  virtual ~Layer4Conn() {
    if (skt_) {
      free(skt_);
      skt_ = nullptr;
    }

    if (stfd_) {
      // we must ensure the close is ok.
      assert(st_netfd_close(stfd_) != -1);
      stfd_ = NULL;
    }
  };

  virtual st_netfd_t GetStfd() = 0;

  CocoSocket *GetCocoSocket() { return skt_; };
  void SetRecvTimeout(uint64_t timeout_us) {
    skt_->set_recv_timeout(timeout_us);
  }
  void SetSendTimeout(uint64_t timeout_us) {
    skt_->set_send_timeout(timeout_us);
  }
  void SetTimeout(uint64_t timeout_us) {
    skt_->set_recv_timeout(timeout_us);
    skt_->set_send_timeout(timeout_us);
  }

protected:
  st_netfd_t stfd_ = nullptr;
  CocoSocket *skt_ = nullptr;
};

class StreamConn : public Layer4Conn {
public:
  StreamConn(st_netfd_t stfd) : Layer4Conn(stfd){};
  virtual ~StreamConn() = default;

  // Layer4Conn method
  st_netfd_t GetStfd() { return stfd_; };

  virtual int Read(void *buf, size_t size, ssize_t *nread) = 0;
  virtual int ReadFully(void *buf, size_t size, ssize_t *nread) = 0;
  virtual int Write(void *buf, size_t size, ssize_t *nwrite) = 0;
  virtual int Writev(const iovec *iov, int iov_size, ssize_t *nwrite) = 0;
};

class DatagramConn : public Layer4Conn {
public:
  DatagramConn(st_netfd_t stfd) : Layer4Conn(stfd){};
  virtual ~DatagramConn() = default;

  // Layer4Conn method
  virtual st_netfd_t GetStfd() { return stfd_; };

  virtual int RecvFrom(void *buf, int size, ssize_t *nread,
                       struct sockaddr *from, int *fromlen) = 0;
  virtual int SendTo(void *buf, int size, ssize_t *nwrite, struct sockaddr *to,
                     int tolen) = 0;
};