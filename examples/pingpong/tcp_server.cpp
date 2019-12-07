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

class PingPongServerConn : public ServerConn
{
private:
    StSocket* skt;
protected:
    virtual int do_cycle();
public:
    PingPongServerConn(ConnMgr *_mgr, st_netfd_t _stfd) : ServerConn(_mgr, _stfd) {
        skt = new StSocket(stfd);
    };
    virtual ~PingPongServerConn() {
        delete skt;
    };
};

int PingPongServerConn::do_cycle()
{
    char buf[1024];
    ssize_t nread = 0;
    ssize_t nwrite = 0;
    int ret = 0;
    while(true) {
        ret = skt->read(buf, sizeof(buf), &nread);
        if (ret != 0) {
            coco_error("read error");
            return -1;
        }
        ssize_t wbytes = 0;
        while(wbytes < nread) {
            ret = skt->write(buf+wbytes, nread-wbytes, &nwrite);
            if(ret != 0) {
                coco_error("write error");
                return -1;
            }
            wbytes += nwrite;
        }
    }
}

ServerConn* tcp_acceptor(ConnMgr* mgr, st_netfd_t stfd)
{
    PingPongServerConn *conn = new PingPongServerConn(mgr, stfd);
    conn->start();

    return conn;
}

int main()
{
    coco_st_init();

    TcpListener *listener  = new TcpListener(TCP_ACCEPTOR(tcp_acceptor), server_ip, port);
    listener->listen();

    coco_uloop(1000*2000);

    delete listener;
    return 0;
}