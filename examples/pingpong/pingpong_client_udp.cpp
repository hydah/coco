#include <iostream>
#include <string>

#include "coco/coco.h"

using namespace std;
using namespace coco;

string server_ip = "127.0.0.1";
int port = 8080;

class PingPongClient {
private:
  std::unique_ptr<UdpConn> conn;
  std::string dst_ip;
  int dst_port;
  int timeout;

public:
  PingPongClient(std::string _server_ip, int _port, int _timeout);
  virtual ~PingPongClient(){};
  int connect();
  int write(char *buf, ssize_t s);
  int read(char *buf, int s, ssize_t *nread);
};

PingPongClient::PingPongClient(std::string _server_ip, int _port,
                               int _timeout) {
  dst_ip = _server_ip;
  dst_port = _port;
  timeout = _timeout;
}

int PingPongClient::connect() {
  return DialUdp(dst_ip, dst_port, timeout, &conn);
}

int PingPongClient::write(char *buf, ssize_t s) {
  ssize_t wbytes = 0;
  ssize_t nwrite = 0;
  int ret = 0;
  while (wbytes < s) {
    ret = conn->Write(buf + wbytes, int(s - wbytes), &nwrite);
    if (ret != 0) {
      coco_trace("write error");
      break;
    }
    wbytes += nwrite;
  }

  return ret;
}

int PingPongClient::read(char *buf, int s, ssize_t *nread) {
  return conn->Read(buf, s, nread);
}

int main() {
  // Ctrl-C ends the loop: it fails the read or sleep in progress and CocoShouldStop() turns
  // true, so the connection is closed properly.
  return CocoRun([]() -> int {
    std::unique_ptr<PingPongClient> client(new PingPongClient(server_ip, port, 1000));
    int ret = client->connect();
    if (ret != COCO_SUCCESS) {
      coco_error("connect server: %s:%d failed", server_ip.c_str(), port);
      return ret;
    }

    char buf[1024] = "hello coco";
    ssize_t nread = 10;
    while (!CocoShouldStop()) {
      ret = client->write(buf, nread);
      if (ret != 0) {
        coco_warn("write error");
        break;
      }
      coco_dbg("write %s", buf);
      ret = client->read(buf, sizeof(buf), &nread);
      if (ret != 0) {
        coco_warn("read error");
        break;
      }
      coco_trace("read %s", buf);
      CocoSleepMs(1000);
    }
    return 0;
  });
}
