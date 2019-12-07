#include "coco/net/tcp_stack.hpp"
#include "coco/base/error.hpp"
#include "coco/base/utils.hpp"
#include "coco/base/log.hpp"
#include <netdb.h>
#include <assert.h>
#include <string.h>

#define SERVER_LISTEN_BACKLOG 512

TcpListener::TcpListener(TCP_ACCEPTOR h, std::string i, int p)
{
    tcp_accept = h;
    ip = i;
    port = p;

    _fd = -1;
    _stfd = NULL;

    coroutine = new StReusableCoroutine("tcp", this);
}

TcpListener::~TcpListener()
{
    coroutine->stop();
    if (coroutine) {
        delete coroutine;
    }

    coco_close_stfd(_stfd);
}

int TcpListener::fd()
{
    return _fd;
}

int TcpListener::listen()
{
    int ret = ERROR_SUCCESS;

    char port_string[8];
    snprintf(port_string, sizeof(port_string), "%d", port);
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = is_ipv6(ip) ? AF_INET6 : AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICHOST;
    addrinfo* result  = NULL;
    if(getaddrinfo(ip.c_str(), port_string, (const addrinfo*)&hints, &result) != 0) {
        ret = ERROR_SYSTEM_IP_INVALID;
        coco_error("bad address. ret=%d", ret);
        return ret;
    }

    if ((_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol)) == -1) {
        ret = ERROR_SOCKET_CREATE;
        coco_error("create linux socket error. port=%d, ret=%d", port, ret);
        freeaddrinfo(result);
        return ret;
    }
    coco_dbg("create linux socket success. port=%d, fd=%d", port, _fd);

    int reuse_socket = 1;
    if (setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_socket, sizeof(int)) == -1) {
        ret = ERROR_SOCKET_SETREUSE;
        coco_error("setsockopt reuse-addr error. port=%d, ret=%d", port, ret);
        freeaddrinfo(result);
        return ret;
    }
    coco_dbg("setsockopt reuse-addr success. port=%d, fd=%d", port, _fd);

    if (bind(_fd, result->ai_addr, result->ai_addrlen) == -1) {
        ret = ERROR_SOCKET_BIND;
        coco_error("bind socket error. ep=%s:%d, ret=%d", ip.c_str(), port, ret);
        freeaddrinfo(result);
        return ret;
    }
    coco_dbg("bind socket success. ep=%s:%d, fd=%d", ip.c_str(), port, _fd);

    if (::listen(_fd, SERVER_LISTEN_BACKLOG) == -1) {
        ret = ERROR_SOCKET_LISTEN;
        coco_error("listen socket error. ep=%s:%d, ret=%d", ip.c_str(), port, ret);
        freeaddrinfo(result);
        return ret;
    }
    coco_dbg("listen socket success. ep=%s:%d, fd=%d", ip.c_str(), port, _fd);

    if ((_stfd = st_netfd_open_socket(_fd)) == NULL){
        ret = ERROR_ST_OPEN_SOCKET;
        coco_error("st_netfd_open_socket open socket failed. ep=%s:%d, ret=%d", ip.c_str(), port, ret);
        freeaddrinfo(result);
        return ret;
    }
    coco_dbg("st open socket success. ep=%s:%d, fd=%d", ip.c_str(), port, _fd);

    if ((ret = coroutine->start()) != ERROR_SUCCESS) {
        coco_error("st_thread_create listen thread error. ep=%s:%d, ret=%d", ip.c_str(), port, ret);
        freeaddrinfo(result);
        return ret;
    }
    coco_dbg("create st listen thread success, ep=%s:%d", ip.c_str(), port);

    freeaddrinfo(result);
    return ret;
}

int TcpListener::cycle()
{
    int ret = ERROR_SUCCESS;
    ServerConn *conn = NULL;

    st_netfd_t client_stfd = st_accept(_stfd, NULL, NULL, ST_UTIME_NO_TIMEOUT);

    if(client_stfd == NULL){
        // ignore error.
        if (errno != EINTR) {
            coco_error("ignore accept thread stoppped for accept client error");
        }
        return ret;
    }
    coco_dbg("get a client. fd=%d", st_netfd_fileno(client_stfd));

    if ((conn = tcp_accept(this, client_stfd)) == NULL) {
        coco_warn("accept client error. ret=%d", ret);
        return ret;
    }

    push(conn);

    return ret;
}

TcpClient::TcpClient(std::string server_ip, int _port, int64_t _timeout)
{
    dst_server = server_ip;
    port = _port;
    timeout = _timeout;
    _stfd = NULL;
}

TcpClient::~TcpClient()
{
    if (skt) {
        delete skt;
    }
    coco_close_stfd(_stfd);
}

int TcpClient::connect()
{
    int ret = ERROR_SUCCESS;


    char port_string[8];
    snprintf(port_string, sizeof(port_string), "%d", port);
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result  = NULL;

    if(getaddrinfo(dst_server.c_str(), port_string, (const addrinfo*)&hints, &result) != 0) {
        ret = ERROR_SYSTEM_IP_INVALID;
        coco_error("dns resolve server error, ip empty. ret=%d", ret);
        return ret;
    }


    // ip v4 or v6
    char ip_c[64];
    int success = getnameinfo(result->ai_addr, result->ai_addrlen,
                                    (char*)&ip_c, sizeof(ip_c),
                                    NULL, 0,
                                    NI_NUMERICHOST);
    if(success != 0) {
        freeaddrinfo(result);
        coco_error("get ip addr from sock addr failed.");
        return ret;
    }

    _fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if(_fd == -1){
        ret = ERROR_SOCKET_CREATE;
        coco_error("[FATAL_SOCKET_CREATE]create socket error. ret=%d", ret);
        freeaddrinfo(result);
        return ret;
    }


    assert(!_stfd);
    _stfd = st_netfd_open_socket(_fd);
    if(_stfd == NULL){
        ret = ERROR_ST_OPEN_SOCKET;
        coco_error("[FATAL_SOCKET_OPEN]st_netfd_open_socket failed. ret=%d", ret);
        ::close(_fd);
        freeaddrinfo(result);
        return ret;
    }


    // connect to server.
    std::string ip = ip_c;
    if (ip.empty()) {
        ret = ERROR_SYSTEM_IP_INVALID;
        coco_error("[FATAL_DNS_RESOLVE]dns resolve server error[%s], ip empty. ret=%d", ip.c_str(), ret);
        goto failed;
    }

    if (st_connect(_stfd, result->ai_addr, result->ai_addrlen, timeout) == -1){
        ret = ERROR_ST_CONNECT;
        coco_error("[WARNING_3_CONNECT_SERVER]connect to server error. ip=%s, port=%d, ret=%d", ip_c, port, ret);
        goto failed;
    }

    coco_info("connect ok. server=%s, ip=%s, port=%d", ip.c_str(), ip_c, port);

    skt = new StSocket(_stfd);
    return ret;

failed:
    coco_close_stfd(_stfd);
    freeaddrinfo(result);
    return ret;
}

int TcpClient::fd()
{
    return _fd;
}