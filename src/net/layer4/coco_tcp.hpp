#pragma once

#include <algorithm>
#include <string>
#include <unistd.h>
#include <vector>

#include "net/coco_socket.hpp"
#include "net/layer4/coco_layer4.hpp"

class TcpConn : public StreamConn {
public:
  TcpConn(st_netfd_t _stfd);
  virtual ~TcpConn() = default;

public:
  virtual std::string RemoteAddr();
};

class TcpListener {
public:
  TcpListener(TcpConn *conn);
  virtual ~TcpListener();

  virtual int Close() { return 0; };
  virtual TcpConn *Accept();
  virtual std::string Addr();
  virtual st_netfd_t GetStfd();
  virtual void SetRecvTimeout(int64_t timeout_us) {
    conn_->SetRecvTimeout(timeout_us);
  };
  virtual void set_send_timeout(int64_t timeout_us) {
    conn_->SetSendTimeout(timeout_us);
  };

private:
  TcpConn *conn_;
};