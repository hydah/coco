#include <iostream>
#include <string>

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "net/coco_socket.hpp"
#include "net/layer7/coco_http.hpp"


using namespace std;

string server_ip = "127.0.0.1";
int port = 9082;

int main() {
  int ret = COCO_SUCCESS;

  CocoInit();


  HttpClient hc;
  hc.Initialize(true, server_ip, port);
  HttpMessage *msg;
  ret = hc.Get("/", "", &msg, "");
  if (ret != COCO_SUCCESS) {
      coco_error("https get error: %d", ret);
  }
  
  std::string body;
  ret = msg->body_read_all(body);
  if (ret != COCO_SUCCESS) {
      coco_error("https body read error: %d", ret);
  } else {
      coco_info("body is %s", body.c_str());
  }

  if(msg) {
      delete msg;
  }
  return 0;
}