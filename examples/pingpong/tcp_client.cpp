#include "coco/net/tcp_stack.hpp"
#include "coco/net/server.hpp"
#include <iostream>
#include "coco/base/log.hpp"
#include <string>
#include "coco/base/st_socket.hpp"
#include "coco/base/error.hpp"

using namespace std;

string server_ip = "127.0.0.1";
int port = 8080;

class PingPongClient : public TcpClient
{
public:
    PingPongClient(std::string _server_ip, int _port, int _timeout) : TcpClient(_server_ip, _port, _timeout) {};
    virtual ~PingPongClient() {};
    int write(char *buf, ssize_t s);
    int read(char *buf, int s, ssize_t *nread);
};

int PingPongClient::write(char *buf, ssize_t s)
{
    ssize_t wbytes = 0;
    ssize_t nwrite = 0;
    int ret = 0;
    while(wbytes < s) {
        ret = skt->write(buf+wbytes, s-wbytes, &nwrite);
        if(ret != 0) {
            coco_trace("write error");
            break;
        }
        wbytes += nwrite;
    }

    return ret;
}

int PingPongClient::read(char *buf, int s, ssize_t *nread)
{
    return skt->read(buf, s, nread);
}

int main()
{
    int ret = ERROR_SUCCESS;

    coco_st_init();

    PingPongClient *client = new PingPongClient(server_ip, port, 1000);
    ret = client->connect();
    if (ret != ERROR_SUCCESS) {
        coco_error("connect server: %s:%d failed", server_ip.c_str(), port);
        return ret;
    }

    char buf[1024] = "hello coco";
    ssize_t nread = 10;
    while(true) {
        ret = client->write(buf, nread);
        if (ret != 0) {
            coco_warn("write error");
            break;
        }
        coco_trace("write %s", buf);
        ret = client->read(buf, sizeof(buf), &nread);
        if (ret != 0) {
            coco_warn("read error");
            break;
        }
        coco_trace("read %s", buf);
    }

    delete client;
    return 0;
}