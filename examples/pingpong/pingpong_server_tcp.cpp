#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "net/coco_socket.hpp"
#include "net/layer4/coco_tcp.hpp"

using namespace std;

string local_ip = "127.0.0.1";
int port = 8080;

class PingPongServer : public ConnRoutine {

public:
  PingPongServer(ConnManager *mgr, std::unique_ptr<TcpConn> conn);
  virtual ~PingPongServer() = default;

  virtual int DoCycle();

private:
  std::unique_ptr<TcpConn> conn_;
};

PingPongServer::PingPongServer(ConnManager *mgr, std::unique_ptr<TcpConn> conn)
    : ConnRoutine(mgr) {
  conn_ = std::move(conn);
}

int PingPongServer::DoCycle() {
  char buf[1024];
  ssize_t nread = 0;
  ssize_t nwrite = 0;
  int ret = 0;
  while (true) {
    ret = conn_->Read(buf, sizeof(buf), &nread);
    if (ret != 0) {
      coco_error("read error");
      return -1;
    }
    ssize_t wbytes = 0;
    while (wbytes < nread) {
      ret = conn_->Write(buf + wbytes, nread - wbytes, &nwrite);
      if (ret != 0) {
        coco_error("write error");
        return -1;
      }
      wbytes += nwrite;
    }
  }
}

class PingPongListener : public ListenRoutine {
public:
  PingPongListener(TcpListener *_l);
  virtual ~PingPongListener();

  virtual int Cycle();

private:
  TcpListener *l_;
  ConnManager *manager_;
};

PingPongListener::PingPongListener(TcpListener *l) { l_ = l; }

PingPongListener::~PingPongListener() {
  if (l_) {
    delete l_;
    l_ = nullptr;
  }

  if (manager_) {
    delete manager_;
    manager_ = nullptr;
  }
}

int PingPongListener::Cycle() {
  while (true) {
    manager_->Destroy();

    std::unique_ptr<TcpConn> p;
    TcpConn *conn = l_->Accept();
    p.reset(conn);
    PingPongServer *pserver = new PingPongServer(manager_, std::move(p));
    pserver->Start();
  }
  return 0;
}

int main() {
  log_level = log_dbg;
  CocoInit();

  TcpListener *l = ListenTcp(local_ip, port);
  if (l == NULL) {
    coco_error("create listen socket failed");
    return -1;
  }
  PingPongListener *pl = new PingPongListener(l);
  pl->Start();

  CocoLoopMs(1000);

  delete pl;
  return 0;
}