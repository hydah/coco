#include <iostream>
#include <string>
#include <vector>
#include "coco/base/log.hpp"
#include "coco/base/st_socket.hpp"
#include "coco/base/error.hpp"
#include "coco/net/net.hpp"
#include <arpa/inet.h>

using namespace std;

string local_ip = "127.0.0.1";
int port = 8080;

// class PingPongServer : public ConnRoutine
// {
// private:
//     ConnMgr<PingPongServer> *mgr;
//     UdpConn *conn;
// public:
//     PingPongServer(ConnMgr<PingPongServer> *_mgr, UdpConn *_conn);
//     virtual ~PingPongServer();

// public:
//     virtual int do_cycle();
//     virtual void on_coroutine_stop();
// };

// PingPongServer::PingPongServer(ConnMgr<PingPongServer> *_mgr, UdpConn *_conn)
// {
//     conn = _conn;
//     mgr = _mgr;
// }

// PingPongServer::~PingPongServer()
// {
//     if(conn) {
//         delete conn;
//     }
// }

// int PingPongServer::do_cycle()
// {
//     char buf[1024];
//     ssize_t nread = 0;
//     ssize_t nwrite = 0;
//     int ret = 0;
//     while(true) {
//         ret = conn->read(buf, sizeof(buf), &nread);
//         if (ret != 0) {
//             coco_error("read error");
//             return -1;
//         }
//         ssize_t wbytes = 0;
//         while(wbytes < nread) {
//             ret = conn->write(buf+wbytes, nread-wbytes, &nwrite);
//             if(ret != 0) {
//                 coco_error("write error");
//                 return -1;
//             }
//             wbytes += nwrite;
//         }
//     }
// }

// void PingPongServer::on_coroutine_stop()
// {
//     mgr->remove(this);
// }


class PingPongListener : public ListenRoutine
{
private:
    UdpListener *l;
public:
    PingPongListener(UdpListener *_l);
    virtual ~PingPongListener();
public:
    virtual int cycle();
};

PingPongListener::PingPongListener(UdpListener *_l)
{
    l = _l;
}

PingPongListener::~PingPongListener()
{
    if(l) {
        delete l;
    }
}

int PingPongListener::cycle()
{
    char buf[1024];
    ssize_t nread = 0;
    ssize_t nwrite = 0;
    struct sockaddr_in addr;
    int len = 2048;
    int ret = 0;
    while(true) {
        ret = l->recvfrom(buf, len, &nread, (struct sockaddr *)&addr, &len);
        coco_trace("read %s", buf);
        l->sendto(buf, (int)nread, &nwrite, (struct sockaddr *)&addr, len);
    }

    return ret;
}

int main()
{
    log_level = log_dbg;
    coco_st_init();

    UdpListener *l = listen_udp(local_ip, port);
    if (l == NULL) {
        coco_error("create listen socket failed");
        return -1;
    }
    PingPongListener *pl = new PingPongListener(l);
    pl->start();

    coco_uloop(1000*2000);

    delete pl;
    return 0;
}