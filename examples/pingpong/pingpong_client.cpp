// #include "coco/net/tcp_stack.hpp"
// #include "coco/net/server.hpp"
#include <iostream>
#include "coco/base/log.hpp"
#include <string>
#include "coco/base/st_socket.hpp"
#include "coco/base/error.hpp"
#include "coco/net/net.hpp"

using namespace std;

string server_ip = "127.0.0.1";
int port = 8080;

class PingPongClient
{
private:
    Conn *conn;
    std::string dst_ip;
    int dst_port;
    int timeout;
public:
    PingPongClient(std::string _server_ip, int _port, int _timeout);
    virtual ~PingPongClient() {};
    int connect();
    int write(char *buf, ssize_t s);
    int read(char *buf, int s, ssize_t *nread);
};

PingPongClient::PingPongClient(std::string _server_ip, int _port, int _timeout)
{
    dst_ip = _server_ip;
    dst_port = _port;
    timeout = _timeout;
}
int PingPongClient::connect()
{
    conn = dial_tcp(dst_ip, dst_port, timeout);
    if (conn == NULL) {
        return -1;
    }
    return 0;
}

int PingPongClient::write(char *buf, ssize_t s)
{
    ssize_t wbytes = 0;
    ssize_t nwrite = 0;
    int ret = 0;
    while(wbytes < s) {
        ret = conn->write(buf+wbytes, s-wbytes, &nwrite);
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
    return conn->read(buf, s, nread);
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
        coco_dbg("write %s", buf);
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