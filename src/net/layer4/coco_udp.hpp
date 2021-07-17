#pragma once

#include <algorithm>
#include <string>
#include <unistd.h>
#include <vector>

#include "net/coco_socket.hpp"
#include "net/layer4/coco_layer4.hpp"

class UdpConn : public DatagramConn {
public:
  UdpConn(st_netfd_t stfd);
  UdpConn(st_netfd_t stfd, struct sockaddr &addr, socklen_t len);
  virtual ~UdpConn() = default;

  virtual int RecvFrom(void *buf, int size, ssize_t *nread,
                       struct sockaddr *from, int *fromlen);
  virtual int SendTo(void *buf, int size, ssize_t *nwrite, struct sockaddr *to,
                     int tolen);

  virtual int Read(void *buf, int size, ssize_t *nread) {
    return RecvFrom(buf, size, nread, &dst_addr, &dst_addr_len);
  };
  virtual int Write(void *buf, int size, ssize_t *nwrite) {
    return SendTo(buf, size, nwrite, &dst_addr, dst_addr_len);
  }

private:
  sockaddr dst_addr;
  int dst_addr_len{0};
};

class UdpListener {
public:
  UdpListener(UdpConn *conn);
  virtual ~UdpListener();

public:
  virtual int close() { return 0; };
  virtual UdpConn *accept() { return NULL; }; // not support accept now
  virtual std::string addr();
  virtual st_netfd_t get_stfd();

  virtual int RecvFrom(void *buf, int size, ssize_t *nread,
                       struct sockaddr *from, int *fromlen);
  virtual int SendTo(void *buf, int size, ssize_t *nwrite, struct sockaddr *to,
                     int tolen);
  virtual void SetRecvTimeout(int64_t timeout_us) {
    conn_->SetRecvTimeout(timeout_us);
  };
  virtual void setSendTimeout(int64_t timeout_us) {
    conn_->SetSendTimeout(timeout_us);
  };

private:
  UdpConn *conn_;
};