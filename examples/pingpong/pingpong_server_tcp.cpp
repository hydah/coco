#include <string>

#include "coco/coco.h"

using namespace std;
using namespace coco;

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
  return CocoRun([]() {
    TcpServer server(PingPong);
    // Serves until Ctrl-C.
    if (server.ListenAndServe(local_ip, port) != COCO_SUCCESS) {
      coco_error("create listen socket failed");
      return -1;
    }
    return 0;
  });
}
