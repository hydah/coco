#ifndef NET_TCP_CLIENT_HPP
#define NET_TCP_CLIENT_HPP

#include "coco/base/st_coroutine.hpp"
#include "coco/base/st_socket.hpp"
#include "coco/net/server.hpp"
#include "coco/net/client.hpp"
#include <string>

typedef ServerConn* (*TCP_ACCEPTOR)(ConnMgr*, st_netfd_t);

class TcpListener : public ConnMgr, public IStCoroutineHandler
{
private:
    int _fd;
    st_netfd_t _stfd;
    StReusableCoroutine* coroutine;
private:
    TCP_ACCEPTOR tcp_accept;
    std::string ip;
    int port;
public:
    TcpListener(TCP_ACCEPTOR h, std::string i, int p);
    virtual ~TcpListener();
public:
    virtual int fd();
public:
    virtual int listen();
public:
    virtual int cycle();
};

class TcpClient : public Client
{
private:
    int _fd;
    st_netfd_t _stfd;
protected:
    std::string ip;
    int port;
    int64_t timeout;
    StSocket *skt;
public:
    TcpClient(std::string i, int p, int64_t _timeout);
    virtual ~TcpClient();
public:
    virtual int fd();
public:
    virtual int connect();
};


#endif