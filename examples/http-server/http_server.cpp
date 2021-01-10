
#include <iostream>
#include "coco/net/http_stack.hpp"
#include "coco/base/log.hpp"
#include "coco/base/st_socket.hpp"
#include "coco/base/error.hpp"
#include "coco/net/net.hpp"

#include <string>
#include <memory>

using namespace std;

class DefHandler : public IHttpHandler
{
public:
    DefHandler();
    virtual ~DefHandler();
public:
    virtual int serve_http(HttpResponseWriter* w, HttpMessage* r);
};

DefHandler::DefHandler()
{
}

DefHandler::~DefHandler()
{
}

int DefHandler::serve_http(HttpResponseWriter* w, HttpMessage* r)
{
    std::string res = "hello world";
    w->header()->set_content_length((int)res.length());
    w->header()->set_content_type("text/jsonp");

    w->write(const_cast<char*>(res.c_str()), (int)res.length());

    return ERROR_SUCCESS;
}

void mainloop()
{
    uint32_t dur = 1000*2000;
    while(true) {
        coco_info("sleep %u us", dur);
        st_usleep(dur);
    }
}

int main()
{
    log_level = log_dbg;
    coco_st_init();

    // run http server
    std::string _ip = "0.0.0.0";
    int32_t _port = 80;

    auto httpServer = std::unique_ptr<HttpServer>(new HttpServer);
    auto _mux = std::unique_ptr<HttpServeMux>(new HttpServeMux());
    _mux->handle("/", new DefHandler());
    httpServer->listenAndServe(_ip, _port, _mux.get());
    httpServer->start();


    mainloop();

    return 0;
}