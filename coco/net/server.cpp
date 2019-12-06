#include "coco/net/server.hpp"
#include "coco/base/error.hpp"
#include "coco/base/log.hpp"
#include <algorithm>

ServerConn::ServerConn(ConnMgr *_mgr, st_netfd_t c)
{
    mgr =  _mgr;
    stfd = c;
    disposed = false;
    coroutine = new StOneCycleCoroutine("conn", this);
}

ServerConn::~ServerConn()
{
    dispose();

    if (coroutine != NULL) {
        delete coroutine;
    }
}

void ServerConn::dispose()
{
    if (disposed) {
        return;
    }

    disposed = true;

    /**
     * when delete the connection, stop the connection,
     * close the underlayer socket, delete the thread.
     */
    close_stfd(stfd);
}

int ServerConn::start()
{
    return coroutine->start();
}

int ServerConn::cycle()
{
    int ret = ERROR_SUCCESS;

    id = get_stid();

    // coco_trace("[TRACE_ANCHOR] SrsConnection: server=[%s:%d], client=[%s:%d]", local_ip.c_str(), local_port, ip.c_str(), port);

    ret = do_cycle();

    // if socket io error, set to closed.
    if (coco_is_client_gracefully_close(ret)) {
        ret = ERROR_SOCKET_CLOSED;
    }

    // success.
    if (ret == ERROR_SUCCESS) {
        coco_trace("client finished.");
    }

    // client close peer.
    if (ret == ERROR_SOCKET_CLOSED) {
        coco_warn("client disconnect peer. ret=%d", ret);
    }

    return ERROR_SUCCESS;
}

void ServerConn::on_coroutine_stop()
{
    // TODO: FIXME: never remove itself, use isolate thread to do cleanup.
    mgr->remove(this);
}

ConnMgr::ConnMgr()
{

}

ConnMgr::~ConnMgr()
{

}

void ConnMgr::push(ServerConn* conn)
{
    conns.push_back(conn);
}

void ConnMgr::remove(ServerConn* conn)
{
    std::vector<ServerConn*>::iterator it = std::find(conns.begin(), conns.end(), conn);

    // removed by destroy, ignore.
    if (it == conns.end()) {
        coco_warn("server moved connection, ignore.");
        return;
    }

    conns.erase(it);

    coco_info("conn removed. conns=%d", (int)conns.size());
    // all connections are created by server,
    // so we free it here.
    if (conn != NULL) {
        delete conn;
    }
}