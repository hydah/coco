#ifndef NET_SERVER_HPP
#define NET_SERVER_HPP

#include "coco/base/st_coroutine.hpp"
#include "coco/base/st_socket.hpp"
#include <string>
#include <vector>
class ConnMgr;

class ServerConn : public virtual IStCoroutineHandler
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
    * the underlayer st fd handler.
    */
    st_netfd_t stfd;
    ConnMgr *mgr;
     /**
     * whether the connection is disposed,
     * when disposed, connection should stop cycle and cleanup itself.
     */
    bool disposed;

public:
    ServerConn(ConnMgr *_mgr, st_netfd_t c);
    virtual ~ServerConn();
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
    virtual void on_coroutine_stop();
protected:
    /**
    * for concrete connection to do the cycle.
    */
    virtual int do_cycle() = 0;
};


/*
 * user defined Listen object should inherited from this
 * to delete conn object
 */

class ConnMgr
{
private:
    std::vector<ServerConn*> conns;

public:
    ConnMgr();
    virtual ~ConnMgr();
public:
    virtual void push(ServerConn* conn);
public:
    virtual void remove(ServerConn* conn);
};

#endif