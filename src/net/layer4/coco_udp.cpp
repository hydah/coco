#include "net/layer4/coco_udp.hpp"

#include <assert.h>
#include <netdb.h>
#include <string.h>
#include <algorithm>

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "utils/utils.hpp"

UdpConn::UdpConn(st_netfd_t stfd) : DatagramConn(stfd) {}

UdpConn::UdpConn(st_netfd_t stfd, struct sockaddr &addr, socklen_t len) : DatagramConn(stfd) {
    memcpy(&dst_addr, &addr, len);
    dst_addr_len = len;
}

UdpListener::UdpListener(UdpConn *conn) {
    coco_dbg("enter udp listener construction");
    conn_ = conn;
}

UdpListener::~UdpListener() {
    if (conn_) {
        delete conn_;
    }
}

std::string UdpListener::addr() { return std::string(); }

st_netfd_t UdpListener::get_stfd() { return conn_->GetStfd(); }

int UdpListener::RecvFrom(void *buf, int size, ssize_t *nread, struct sockaddr *from,
                          int *fromlen) {
    return conn_->RecvFrom(buf, size, nread, from, fromlen);
}

int UdpListener::SendTo(void *buf, int size, ssize_t *nwrite, struct sockaddr *to, int tolen) {
    return conn_->SendTo(buf, size, nwrite, to, tolen);
}

// helper function
UdpListener *ListenUdp(std::string local_ip, int local_port) {
    int ret = COCO_SUCCESS;
    int _fd = -1;
    st_netfd_t stfd = NULL;

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
        coco_error("create linux socket error. udp[%s:%d], ret=%d", local_ip.c_str(), local_port,
                   ret);
        freeaddrinfo(result);
        return NULL;
    }
    coco_dbg("create linux socket success. udp[%s:%d], fd=%d", local_ip.c_str(), local_port, _fd);

    if (bind(_fd, result->ai_addr, result->ai_addrlen) == -1) {
        ret = ERROR_SOCKET_BIND;
        coco_error("bind socket error. udp[%s:%d], ret=%d", local_ip.c_str(), local_port, ret);
        freeaddrinfo(result);
        return NULL;
    }
    coco_dbg("bind socket success. udp[%s:%d], fd=%d", local_ip.c_str(), local_port, _fd);

    if ((stfd = st_netfd_open_socket(_fd)) == NULL) {
        ret = ERROR_ST_OPEN_SOCKET;
        coco_error("st_netfd_open_socket open socket failed. ep=%s:%d, ret=%d", local_ip.c_str(),
                   local_port, ret);
        freeaddrinfo(result);
        return NULL;
    }
    coco_dbg("st open socket success. ep=%s:%d, fd=%d, stfd: %p", local_ip.c_str(), local_port, _fd,
             stfd);

    UdpListener *l = new UdpListener(new UdpConn(stfd));
    freeaddrinfo(result);

    return l;
}

UdpConn *DialUdp(std::string dst_ip, int dst_port, int timeout) {
    int ret = COCO_SUCCESS;
    int _fd = -1;
    st_netfd_t stfd = NULL;
    UdpConn *conn = nullptr;
    // coco_trace("ip: %s, port is %d", dst_ip.c_str(), dst_port);

    char port_string[8];
    snprintf(port_string, sizeof(port_string), "%d", dst_port);
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *result = NULL;

    if (getaddrinfo(dst_ip.c_str(), port_string, (const addrinfo *)&hints, &result) != 0) {
        ret = ERROR_SYSTEM_IP_INVALID;
        coco_error("dns resolve server error, ip empty. ret=%d", ret);
        return NULL;
    }

    // ip v4 or v6
    char ip_c[64];
    int success = getnameinfo(result->ai_addr, result->ai_addrlen, (char *)&ip_c, sizeof(ip_c),
                              NULL, 0, NI_NUMERICHOST);
    if (success != 0) {
        freeaddrinfo(result);
        coco_error("get ip addr from sock addr failed.");
        return NULL;
    }

    _fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (_fd == -1) {
        ret = ERROR_SOCKET_CREATE;
        coco_error("[FATAL_SOCKET_CREATE]create socket error. ret=%d", ret);
        freeaddrinfo(result);
        return NULL;
    }

    assert(!stfd);
    stfd = st_netfd_open_socket(_fd);
    if (stfd == NULL) {
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

    conn = new UdpConn(stfd, *result->ai_addr, result->ai_addrlen);
    conn->SetSendTimeout(timeout);
    return conn;

failed:
    if (stfd) {
        // we must ensure the close is ok.
        assert(st_netfd_close(stfd) != -1);
        stfd = NULL;
    }
    freeaddrinfo(result);
    return NULL;
}