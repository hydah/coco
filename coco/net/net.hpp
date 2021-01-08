#ifndef NET_NET_HPP
#define NET_NET_HPP

#include <string>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include "coco/base/st_socket.hpp"

class TcpConn
{
private:
    st_netfd_t _stfd;
    StSocket* skt;

public:
    TcpConn(st_netfd_t _stfd);
    virtual ~TcpConn();
public:
    /**
     * @param nread, the actual read bytes, ignore if NULL.
     */
    virtual int read(void *buf, size_t size, ssize_t *nread);
    virtual int read_fully(void *buf, size_t size, ssize_t *nread);
    /**
     * @param nwrite, the actual write bytes, ignore if NULL.
     */
    virtual int write(void *buf, size_t size, ssize_t *nwrite);
    virtual int writev(const iovec *iov, int iov_size, ssize_t *nwrite);
    virtual st_netfd_t get_stfd();
    virtual StSocket* getStSocket() { return skt;};
};

class TcpListener
{
private:
    TcpConn* conn;
public:
    TcpListener(TcpConn *_conn);
    virtual ~TcpListener();

public:
    virtual int close() {return 0;};
    virtual TcpConn* accept();
    virtual std::string addr();
    virtual st_netfd_t get_stfd();
};

extern TcpListener* listen_tcp(std::string local_ip, int local_port);
extern TcpConn* dial_tcp(std::string dst_ip, int dst_port, int timeout);

template <class T>
class ConnMgr
{
protected:
    std::vector<T*> conns;

public:
    ConnMgr(){};
    virtual ~ConnMgr()
    {
        typename std::vector<T*>::iterator it;
        for (it = conns.begin(); it != conns.end(); it = it++) {
            if (*it != NULL) {
                delete *it;
            }
        }
        conns.clear();
    };
public:
    virtual void push(T* conn)
    {
        conns.push_back(conn);
    };
public:
    virtual void remove(T* conn)
    {
         typename std::vector<T*>::iterator it = std::find(conns.begin(), conns.end(), conn);

        // removed by destroy, ignore.
        if (it == conns.end()) {
            // coco_warn("server moved connection, ignore.");
            return;
        }

        conns.erase(it);

        // coco_info("conn removed. conns=%d", (int)conns.size());
        // all connections are created by server,
        // so we free it here.
        if (conn != NULL) {
            delete conn;
        }
    };
};

class ListenRoutine : public IStCoroutineHandler
{
private:
    StReusableCoroutine* coroutine;

public:
    ListenRoutine();
    virtual ~ListenRoutine();
public:
    virtual int cycle() = 0;
    virtual int start();
};

class ConnRoutine : public IStCoroutineHandler
{
private:
    /**
    * each connection start a green thread,
    * when thread stop, the connection will be delete by server.
    */
    StOneCycleCoroutine* coroutine;
    /**
    * the id of connection.
    */
    int id;

protected:
     /**
     * whether the connection is disposed,
     * when disposed, connection should stop cycle and cleanup itself.
     */
    bool disposed;

public:
    ConnRoutine();
    virtual ~ConnRoutine();
public:
    /**
     * to dipose the connection.
     */
    virtual void dispose();
    /**
    * start the client green thread.
    * when server get a client from listener,
    * 1. server will create an concrete connection(for instance, RTMP connection),
    * 2. then add connection to its connection manager,
    * 3. start the client thread by invoke this start()
    * when client cycle thread stop, invoke the on_thread_stop(), which will use server
    * to remove the client by server->remove(this).
    */
    virtual int start();
// interface ISrsOneCycleThreadHandler
public:
    /**
    * the thread cycle function,
    * when serve connection completed, terminate the loop which will terminate the thread,
    * thread will invoke the on_thread_stop() when it terminated.
    */
    virtual int cycle();
    /**
    * when the coroutine cycle finished, thread will invoke the on_coroutine_stop(),
    * which will remove self from server, server will remove the connection from manager
    * then delete the connection.
    */
    virtual void on_coroutine_stop() = 0;
protected:
    /**
    * for concrete connection to do the cycle.
    */
    virtual int do_cycle() = 0;
};


class UdpConn
{
private:
    st_netfd_t _stfd;
    StSocket* skt;
    struct sockaddr *dst_addr;
    int dst_addr_len;

public:
    UdpConn(st_netfd_t _stfd);
    UdpConn(st_netfd_t _stfd, struct sockaddr* _addr, socklen_t len);
    virtual ~UdpConn();
public:
    virtual int recvfrom(void *buf, int size, ssize_t *nread, struct sockaddr *from, int *fromlen);
    virtual int sendto(void *buf, int size, ssize_t *nwrite, struct sockaddr *to, int tolen);
    virtual int read(void *buf, int size, ssize_t *nread);
    virtual int write(void *buf, int size, ssize_t *nwrite);
    virtual st_netfd_t get_stfd();
};
class UdpListener
{
private:
    UdpConn* conn;
public:
    UdpListener(UdpConn* _conn);
    virtual ~UdpListener();

public:
    virtual int close() {return 0;};
    virtual UdpConn* accept() {return NULL;}; // not support accept now
    virtual std::string addr();
    virtual st_netfd_t get_stfd();

    virtual int recvfrom(void *buf, int size, ssize_t *nread, struct sockaddr *from, int *fromlen);
    virtual int sendto(void *buf, int size, ssize_t *nwrite, struct sockaddr *to, int tolen);
};

extern UdpListener* listen_udp(std::string local_ip, int local_port);
extern UdpConn* dial_udp(std::string dst_ip, int dst_port, int timeout);

#endif // end of NET_NET_HPP