#include "coco/net/net.hpp"
#include "coco/base/log.hpp"
#include "coco/base/error.hpp"
#include "coco/base/utils.hpp"
#include <netdb.h>
#include <algorithm>
#include <string.h>
#include <assert.h>
#define SERVER_LISTEN_BACKLOG 512

/* TcpConn */
TcpConn::TcpConn(st_netfd_t stfd)
{
    _stfd = stfd;
    skt = new StSocket(_stfd);
}

TcpConn::~TcpConn()
{
    if(skt) {
        delete skt;
    }
    coco_close_stfd(_stfd);
}

int TcpConn::read(void* buf, size_t size, ssize_t* nread)
{
    return skt->read(buf, size, nread);
}

int TcpConn::read_fully(void* buf, size_t size, ssize_t* nread)
{
    return skt->read_fully(buf, size, nread);
}

int TcpConn::write(void* buf, size_t size, ssize_t* nwrite)
{
    return skt->write(buf, size, nwrite);
}

int TcpConn::writev(const iovec *iov, int iov_size, ssize_t* nwrite)
{
    return skt->writev(iov, iov_size, nwrite);
}

st_netfd_t TcpConn::get_stfd()
{
    return _stfd;
}

/* TcpListener */
TcpListener::TcpListener(TcpConn* _conn)
{
    coco_dbg("enter listener construction");
    conn = _conn;
}

TcpListener::~TcpListener()
{
    if(conn) {
        delete conn;
    }
}

TcpConn* TcpListener::accept()
{
    st_netfd_t _stfd = get_stfd();
    st_netfd_t client_stfd = st_accept(_stfd, NULL, NULL, ST_UTIME_NO_TIMEOUT);
    if(client_stfd == NULL){
        // ignore error.
        if (errno != EINTR) {
            coco_error("ignore accept thread stoppped for accept client error");
        }
        return NULL;
    }

    coco_trace("get a client. fd=%d", st_netfd_fileno(client_stfd));
    return new TcpConn(client_stfd);
}

std::string TcpListener::addr()
{
    return std::string();
}

st_netfd_t TcpListener::get_stfd()
{
     return conn->get_stfd();
}

ListenRoutine::ListenRoutine()
{
    coroutine = new StReusableCoroutine("tcp", this);
}

ListenRoutine::~ListenRoutine()
{
    coroutine->stop();
    if (coroutine) {
        delete coroutine;
    }
}

int ListenRoutine::start()
{
    return coroutine->start();
}


ConnRoutine::ConnRoutine()
{
    disposed = false;
    coroutine = new StOneCycleCoroutine("conn", this);
}

ConnRoutine:: ~ConnRoutine()
{
    dispose();

    if (coroutine != NULL) {
        delete coroutine;
    }
}

void ConnRoutine::dispose()
{
    if (disposed) {
        return;
    }

    disposed = true;
}

int ConnRoutine::start()
{
    return coroutine->start();
}

int ConnRoutine::cycle()
{
    int ret = ERROR_SUCCESS;

    id = coco_get_stid();

    coco_trace("[TRACE_ANCHOR] Connection");

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

TcpListener* listen_tcp(std::string local_ip, int local_port)
{
    int ret = ERROR_SUCCESS;
    int _fd = -1;
    st_netfd_t _stfd = NULL;

    char port_string[8];
    snprintf(port_string, sizeof(port_string), "%d", local_port);
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = is_ipv6(local_ip) ? AF_INET6 : AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo *result = NULL;
    if (getaddrinfo(local_ip.c_str(), port_string, (const addrinfo *)&hints, &result) != 0) {
        ret = ERROR_SYSTEM_IP_INVALID;
        coco_error("bad address. ret=%d", ret);
        return NULL;
    }

    if ((_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol)) == -1) {
        ret = ERROR_SOCKET_CREATE;
        coco_error("create linux socket error. tcp[%s:%d], ret=%d", local_ip.c_str(), local_port, ret);
        freeaddrinfo(result);
        return NULL;
    }
    coco_dbg("create linux socket success. tcp[%s:%d], fd=%d", local_ip.c_str(), local_port, _fd);

    int reuse_socket = 1;
    if (setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_socket, sizeof(int)) == -1) {
        ret = ERROR_SOCKET_SETREUSE;
        coco_error("setsockopt reuse-addr error. port=%d, ret=%d", local_port, ret);
        freeaddrinfo(result);
        return NULL;
    }
    coco_dbg("setsockopt reuse-addr success. port=%d, fd=%d", local_port, _fd);

    if (bind(_fd, result->ai_addr, result->ai_addrlen) == -1)
    {
        ret = ERROR_SOCKET_BIND;
        coco_error("bind socket error. ep=%s:%d, ret=%d", local_ip.c_str(), local_port, ret);
        freeaddrinfo(result);
        return NULL;
    }
    coco_dbg("bind socket success. ep=%s:%d, fd=%d", local_ip.c_str(), local_port, _fd);

    if (::listen(_fd, SERVER_LISTEN_BACKLOG) == -1)
    {
        ret = ERROR_SOCKET_LISTEN;
        coco_error("listen socket error. ep=%s:%d, ret=%d", local_ip.c_str(), local_port, ret);
        freeaddrinfo(result);
        return NULL;
    }
    coco_dbg("listen socket success. ep=%s:%d, fd=%d", local_ip.c_str(), local_port, _fd);

    if ((_stfd = st_netfd_open_socket(_fd)) == NULL) {
        ret = ERROR_ST_OPEN_SOCKET;
        coco_error("st_netfd_open_socket open socket failed. ep=%s:%d, ret=%d", local_ip.c_str(), local_port, ret);
        freeaddrinfo(result);
        return NULL;
    }
    coco_dbg("st open socket success. ep=%s:%d, fd=%d, stfd: %p", local_ip.c_str(), local_port, _fd, _stfd);

    TcpListener *l = new TcpListener(new TcpConn(_stfd));
    freeaddrinfo(result);

    return l;
}


TcpConn* dial_tcp(std::string dst_ip, int dst_port, int timeout)
{
    int ret = ERROR_SUCCESS;
    int _fd = -1;
    st_netfd_t _stfd = NULL;
    coco_trace("port is %d", dst_port);


    char port_string[8];
    snprintf(port_string, sizeof(port_string), "%d", dst_port);
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result  = NULL;

    if(getaddrinfo(dst_ip.c_str(), port_string, (const addrinfo*)&hints, &result) != 0) {
        ret = ERROR_SYSTEM_IP_INVALID;
        coco_error("dns resolve server error, ip empty. ret=%d", ret);
        return NULL;
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
        return NULL;
    }

    _fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if(_fd == -1){
        ret = ERROR_SOCKET_CREATE;
        coco_error("[FATAL_SOCKET_CREATE]create socket error. ret=%d", ret);
        freeaddrinfo(result);
        return NULL;
    }


    assert(!_stfd);
    _stfd = st_netfd_open_socket(_fd);
    if(_stfd == NULL){
        ret = ERROR_ST_OPEN_SOCKET;
        coco_error("st_netfd_open_socket failed. ret=%d", ret);
        ::close(_fd);
        freeaddrinfo(result);
        return NULL;
    }


    // connect to server.
    std::string ip = ip_c;
    if (ip.empty()) {
        ret = ERROR_SYSTEM_IP_INVALID;
        coco_error("dns resolve server error[%s], ip empty. ret=%d", ip.c_str(), ret);
        goto failed;
    }

    if (st_connect(_stfd, result->ai_addr, result->ai_addrlen, timeout) == -1){
        ret = ERROR_ST_CONNECT;
        coco_error("connect to server error. ip=%s, port=%d, ret=%d", ip_c, dst_port, ret);
        goto failed;
    }

    coco_info("connect ok. server=%s, ip=%s, port=%d", ip.c_str(), ip_c, dst_port);

    return new TcpConn(_stfd);

failed:
    coco_close_stfd(_stfd);
    freeaddrinfo(result);
    return NULL;
}


/* UdpConn */
UdpConn::UdpConn(st_netfd_t stfd)
{
    _stfd = stfd;
    skt = new StSocket(_stfd);
    dst_addr = NULL;
    dst_addr_len = 0;
}

UdpConn::UdpConn(st_netfd_t stfd, struct sockaddr* _addr, socklen_t len)
{
    _stfd = stfd;
    skt = new StSocket(_stfd);
    dst_addr = (struct sockaddr*)malloc(len);
    memcpy(dst_addr, _addr, len);
    dst_addr_len = len;
}

UdpConn::~UdpConn()
{
    if(skt) {
        delete skt;
        skt = NULL;
    }
    coco_close_stfd(_stfd);
    if(dst_addr) {
        delete dst_addr;
        dst_addr = NULL;
    }
}

int UdpConn::recvfrom(void* buf, int size, ssize_t* nread, struct sockaddr *from, int *fromlen)
{
    return skt->recvfrom(buf, size, nread, from, fromlen);
}

int UdpConn::sendto(void* buf, int size, ssize_t* nwrite, struct sockaddr *to, int tolen)
{
    return skt->sendto(buf, size, nwrite, to, tolen);
}

int UdpConn::read(void* buf, int size, ssize_t* nread)
{
    int fromlen;
    return skt->recvfrom(buf, size, nread, dst_addr, &fromlen);
}

int UdpConn::write(void* buf, int size, ssize_t* nwrite)
{
    return skt->sendto(buf, size, nwrite, dst_addr, dst_addr_len);
}


st_netfd_t UdpConn::get_stfd()
{
    return _stfd;
}


/* UdpListener */
UdpListener::UdpListener(UdpConn* _conn)
{
    coco_dbg("enter udp listener construction");
    conn = _conn;
}

UdpListener::~UdpListener()
{
    if(conn) {
        delete conn;
    }
}

std::string UdpListener::addr()
{
    return std::string();
}

st_netfd_t UdpListener::get_stfd()
{
     return conn->get_stfd();
}

int UdpListener::recvfrom(void* buf, int size, ssize_t* nread, struct sockaddr *from, int *fromlen)
{
    return conn->recvfrom(buf, size, nread, from, fromlen);
}

int UdpListener::sendto(void* buf, int size, ssize_t* nwrite, struct sockaddr *to, int tolen)
{
    return conn->sendto(buf, size, nwrite, to, tolen);
}

UdpListener* listen_udp(std::string local_ip, int local_port)
{
    int ret = ERROR_SUCCESS;
    int _fd = -1;
    st_netfd_t _stfd = NULL;

    char port_string[8];
    snprintf(port_string, sizeof(port_string), "%d", local_port);
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = is_ipv6(local_ip) ? AF_INET6 : AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo *result = NULL;
    if (getaddrinfo(local_ip.c_str(), port_string, (const addrinfo *)&hints, &result) != 0) {
        ret = ERROR_SYSTEM_IP_INVALID;
        coco_error("bad address. ret=%d", ret);
        return NULL;
    }

    if ((_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol)) == -1) {
        ret = ERROR_SOCKET_CREATE;
        coco_error("create linux socket error. udp[%s:%d], ret=%d", local_ip.c_str(), local_port, ret);
        freeaddrinfo(result);
        return NULL;
    }
    coco_dbg("create linux socket success. udp[%s:%d], fd=%d", local_ip.c_str(), local_port, _fd);

    if (bind(_fd, result->ai_addr, result->ai_addrlen) == -1)
    {
        ret = ERROR_SOCKET_BIND;
        coco_error("bind socket error. udp[%s:%d], ret=%d", local_ip.c_str(), local_port, ret);
        freeaddrinfo(result);
        return NULL;
    }
    coco_dbg("bind socket success. udp[%s:%d], fd=%d", local_ip.c_str(), local_port, _fd);

    if ((_stfd = st_netfd_open_socket(_fd)) == NULL) {
        ret = ERROR_ST_OPEN_SOCKET;
        coco_error("st_netfd_open_socket open socket failed. ep=%s:%d, ret=%d", local_ip.c_str(), local_port, ret);
        freeaddrinfo(result);
        return NULL;
    }
    coco_dbg("st open socket success. ep=%s:%d, fd=%d, stfd: %p", local_ip.c_str(), local_port, _fd, _stfd);

    UdpListener *l = new UdpListener(new UdpConn(_stfd));
    freeaddrinfo(result);

    return l;
}

UdpConn* dial_udp(std::string dst_ip, int dst_port, int timeout)
{
    int ret = ERROR_SUCCESS;
    int _fd = -1;
    st_netfd_t _stfd = NULL;
    UdpConn* conn = nullptr;
    coco_trace("ip: %s, port is %d", dst_ip.c_str(), dst_port);


    char port_string[8];
    snprintf(port_string, sizeof(port_string), "%d", dst_port);
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result  = NULL;

    if(getaddrinfo(dst_ip.c_str(), port_string, (const addrinfo*)&hints, &result) != 0) {
        ret = ERROR_SYSTEM_IP_INVALID;
        coco_error("dns resolve server error, ip empty. ret=%d", ret);
        return NULL;
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
        return NULL;
    }

    _fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if(_fd == -1){
        ret = ERROR_SOCKET_CREATE;
        coco_error("[FATAL_SOCKET_CREATE]create socket error. ret=%d", ret);
        freeaddrinfo(result);
        return NULL;
    }


    assert(!_stfd);
    _stfd = st_netfd_open_socket(_fd);
    if(_stfd == NULL){
        ret = ERROR_ST_OPEN_SOCKET;
        coco_error("st_netfd_open_socket failed. ret=%d", ret);
        ::close(_fd);
        freeaddrinfo(result);
        return NULL;
    }


    // connect to server.
    std::string ip = ip_c;
    if (ip.empty()) {
        ret = ERROR_SYSTEM_IP_INVALID;
        coco_error("dns resolve server error[%s], ip empty. ret=%d", ip.c_str(), ret);
        goto failed;
    }

    conn = new UdpConn(_stfd, result->ai_addr, result->ai_addrlen);
    conn->set_timeout(timeout);
    return conn;

failed:
    coco_close_stfd(_stfd);
    freeaddrinfo(result);
    return NULL;
}