#include <arpa/inet.h>

#include <iostream>
#include <string>
#include <vector>

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "net/coco_socket.hpp"
#include "net/layer4/coco_udp.hpp"

using namespace std;

string local_ip = "127.0.0.1";
int port = 8080;

class PingPongListener : public ListenRoutine {
public:
  PingPongListener(UdpListener *l);
  virtual ~PingPongListener();

  virtual int Cycle();

private:
  UdpListener *l_;
};

PingPongListener::PingPongListener(UdpListener *l) { l_ = l_; }

PingPongListener::~PingPongListener() {
  if (l_) {
    delete l_;
    l_ = nullptr;
  }
}

int PingPongListener::Cycle() {
  char buf[1024];
  ssize_t nread = 0;
  ssize_t nwrite = 0;
  struct sockaddr_in addr;
  int len = 2048;
  int ret = 0;
  while (true) {
    ret = l_->RecvFrom(buf, len, &nread, (struct sockaddr *)&addr, &len);
    coco_trace("read %s", buf);
    l_->SendTo(buf, (int)nread, &nwrite, (struct sockaddr *)&addr, len);
  }

  return ret;
}

int main() {
  log_level = log_dbg;
  CocoInit();

  UdpListener *l = ListenUdp(local_ip, port);
  if (l == nullptr) {
    coco_error("create listen socket failed");
    return -1;
  }
  PingPongListener *pl = new PingPongListener(l);
  pl->Start();

  CocoLoopMs(1000);

  delete pl;
  return 0;
}