#include <string>

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "server/coco_tcp_server.hpp"

using namespace std;

string local_ip = "127.0.0.1";
int port = 8080;

// Echoes every read back to the peer until it closes or the server stops.
int PingPong(StreamConn &conn) {
  char buf[1024];
  ssize_t nread = 0;
  int ret = COCO_SUCCESS;
  while ((ret = conn.Read(buf, sizeof(buf), &nread)) == COCO_SUCCESS) {
    if ((ret = conn.Write(buf, nread, nullptr)) != COCO_SUCCESS) {
      coco_error("write error");
      break;
    }
  }
  return ret;
}

int main() {
  log_level = log_dbg;
  CocoInit();

  TcpServer server(PingPong);
  if (server.ListenAndServe(local_ip, port) != COCO_SUCCESS) {
    coco_error("create listen socket failed");
    return -1;
  }

  CocoLoopMs(1000);
  return 0;
}
