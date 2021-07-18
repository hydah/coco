#include <iostream>
#include <memory>
#include <string>

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "net/coco_socket.hpp"
#include "net/layer7/coco_http.hpp"

using namespace std;

class DefHandler : public IHttpHandler {
public:
  DefHandler();
  virtual ~DefHandler();

public:
  virtual int serve_http(HttpResponseWriter *w, HttpMessage *r);
};

DefHandler::DefHandler() {}

DefHandler::~DefHandler() {}

int DefHandler::serve_http(HttpResponseWriter *w, HttpMessage *r) {
  std::string res = "hello world";
  w->header()->set_content_length((int)res.length());
  w->header()->set_content_type("text/jsonp");

  w->Write(const_cast<char *>(res.c_str()), (int)res.length());

  return COCO_SUCCESS;
}

int main() {
  log_level = log_dbg;
  CocoInit();

  // run http server
  std::string _ip = "0.0.0.0";
  int32_t _port = 9082;

  // if https is true, start as https server, or http server
  auto httpServer = std::unique_ptr<HttpServer>(new HttpServer(true));
  auto _mux = std::unique_ptr<HttpServeMux>(new HttpServeMux());
  _mux->handle("/", new DefHandler());
  if (httpServer->ListenAndServe(_ip, _port, _mux.get()) != 0) {
    coco_error("listen failed");
    return -1;
  }
  httpServer->Start();

  CocoLoopMs(1000);

  return 0;
}