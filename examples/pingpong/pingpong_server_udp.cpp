#include <arpa/inet.h>

#include <iostream>
#include <string>
#include <vector>

#include "coco/coco.h"

using namespace std;
using namespace coco;

string local_ip = "127.0.0.1";
int port = 8080;

class PingPongListener : public ListenRoutine {
public:
  explicit PingPongListener(std::unique_ptr<UdpListener> l);
  virtual ~PingPongListener();

  virtual int Cycle();

private:
  std::unique_ptr<UdpListener> l_;
};

PingPongListener::PingPongListener(std::unique_ptr<UdpListener> l) : l_(std::move(l)) {}

PingPongListener::~PingPongListener() { Stop(); }

int PingPongListener::Cycle() {
  char buf[1024];
  ssize_t nread = 0;
  ssize_t nwrite = 0;
  struct sockaddr_storage addr;
  int ret = 0;
  while (!ShouldTermCycle()) {
    int addrlen = sizeof(addr);
    // One byte is kept for the terminating NUL.
    ret = l_->RecvFrom(buf, sizeof(buf) - 1, &nread, (struct sockaddr *)&addr, &addrlen);
    if (ret != COCO_SUCCESS) {
      continue;
    }
    buf[nread] = '\0';
    coco_trace("read from: %s, size: %d, buf: %s",
               FormatSockaddr((struct sockaddr *)&addr, addrlen).c_str(), int(nread), buf);
    l_->SendTo(buf, (int)nread, &nwrite, (struct sockaddr *)&addr, addrlen);
  }

  return ret;
}

int main() {
  log_level = log_dbg;
  std::unique_ptr<UdpListener> l;
  int ret = ListenUdp(local_ip, port, &l);
  if (ret != COCO_SUCCESS) {
    coco_error("create listen socket failed. ret=%d", ret);
    return -1;
  }
  PingPongListener *pl = new PingPongListener(std::move(l));
  pl->Start();

  CocoWaitForShutdown();

  delete pl;
  return 0;
}