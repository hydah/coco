#include "net/layer4/coco_tcp.hpp"

#include <algorithm>
#include <assert.h>
#include <netdb.h>
#include <string.h>

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "utils/utils.hpp"

#define SERVER_LISTEN_BACKLOG 512

TcpConn::TcpConn(st_netfd_t stfd) : StreamConn(stfd) {}

std::string TcpConn::RemoteAddr() {
  auto fd = skt_->get_osfd();
  return GetRemoteAddr(fd);
}

/* TcpListener */
TcpListener::TcpListener(TcpConn *conn) {
  coco_dbg("enter listener construction");
  conn_ = conn;
}

TcpListener::~TcpListener() {
  if (conn_) {
    delete conn_;
  }
}

TcpConn *TcpListener::Accept() {
  st_netfd_t stfd = GetStfd();
  st_netfd_t client_stfd = st_accept(stfd, NULL, NULL, ST_UTIME_NO_TIMEOUT);
  if (client_stfd == NULL) {
    // ignore error.
    if (errno != EINTR) {
      coco_error("ignore accept thread stoppped for accept client error");
    }
    return NULL;
  }
  auto fd = st_netfd_fileno(client_stfd);
  coco_trace("get a client. fd=%d, remote addr: %s", fd, GetRemoteAddr(fd).c_str());
  return new TcpConn(client_stfd);
}

std::string TcpListener::Addr() { return std::string(); }

st_netfd_t TcpListener::GetStfd() { return conn_->GetStfd(); }

TcpListener *ListenTcp(std::string local_ip, int local_port) {
  int ret = COCO_SUCCESS;
  int _fd = -1;
  st_netfd_t stfd = nullptr;

  char port_string[8];
  snprintf(port_string, sizeof(port_string), "%d", local_port);
  addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = is_ipv6(local_ip) ? AF_INET6 : AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST;
  addrinfo *result = NULL;
  if (getaddrinfo(local_ip.c_str(), port_string, (const addrinfo *)&hints,
                  &result) != 0) {
    ret = ERROR_SYSTEM_IP_INVALID;
    coco_error("bad address. ret=%d", ret);
    return NULL;
  }

  if ((_fd = socket(result->ai_family, result->ai_socktype,
                    result->ai_protocol)) == -1) {
    ret = ERROR_SOCKET_CREATE;
    coco_error("create linux socket error. tcp[%s:%d], ret=%d",
               local_ip.c_str(), local_port, ret);
    freeaddrinfo(result);
    return NULL;
  }
  coco_dbg("create linux socket success. tcp[%s:%d], fd=%d", local_ip.c_str(),
           local_port, _fd);

  int reuse_socket = 1;
  if (setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_socket, sizeof(int)) ==
      -1) {
    ret = ERROR_SOCKET_SETREUSE;
    coco_error("setsockopt reuse-addr error. port=%d, ret=%d", local_port, ret);
    freeaddrinfo(result);
    return NULL;
  }
  coco_dbg("setsockopt reuse-addr success. port=%d, fd=%d", local_port, _fd);

  if (bind(_fd, result->ai_addr, result->ai_addrlen) == -1) {
    ret = ERROR_SOCKET_BIND;
    coco_error("bind socket error. ep=%s:%d, ret=%d", local_ip.c_str(),
               local_port, ret);
    freeaddrinfo(result);
    return NULL;
  }
  coco_dbg("bind socket success. ep=%s:%d, fd=%d", local_ip.c_str(), local_port,
           _fd);

  if (::listen(_fd, SERVER_LISTEN_BACKLOG) == -1) {
    ret = ERROR_SOCKET_LISTEN;
    coco_error("listen socket error. ep=%s:%d, ret=%d", local_ip.c_str(),
               local_port, ret);
    freeaddrinfo(result);
    return NULL;
  }
  coco_dbg("listen socket success. ep=%s:%d, fd=%d", local_ip.c_str(),
           local_port, _fd);

  if ((stfd = st_netfd_open_socket(_fd)) == NULL) {
    ret = ERROR_ST_OPEN_SOCKET;
    coco_error("st_netfd_open_socket open socket failed. ep=%s:%d, ret=%d",
               local_ip.c_str(), local_port, ret);
    freeaddrinfo(result);
    return NULL;
  }
  coco_dbg("st open socket success. ep=%s:%d, fd=%d, stfd: %p",
           local_ip.c_str(), local_port, _fd, stfd);

  TcpListener *l = new TcpListener(new TcpConn(stfd));
  freeaddrinfo(result);

  return l;
}

TcpConn *DialTcp(std::string dst_ip, int dst_port, int timeout) {
  int ret = COCO_SUCCESS;
  int _fd = -1;
  st_netfd_t stfd = NULL;
  coco_trace("port is %d", dst_port);

  char port_string[8];
  snprintf(port_string, sizeof(port_string), "%d", dst_port);
  addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *result = NULL;

  if (getaddrinfo(dst_ip.c_str(), port_string, (const addrinfo *)&hints,
                  &result) != 0) {
    ret = ERROR_SYSTEM_IP_INVALID;
    coco_error("dns resolve server error, ip empty. ret=%d", ret);
    return NULL;
  }

  // ip v4 or v6
  char ip_c[64];
  int success = getnameinfo(result->ai_addr, result->ai_addrlen, (char *)&ip_c,
                            sizeof(ip_c), NULL, 0, NI_NUMERICHOST);
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
    coco_error("dns resolve server error[%s], ip empty. ret=%d", ip.c_str(),
               ret);
    goto failed;
  }

  if (st_connect(stfd, result->ai_addr, result->ai_addrlen, timeout) == -1) {
    ret = ERROR_ST_CONNECT;
    coco_error("connect to server error. ip=%s, port=%d, ret=%d", ip_c,
               dst_port, ret);
    goto failed;
  }

  coco_info("connect ok. server=%s, ip=%s, port=%d", ip.c_str(), ip_c,
            dst_port);

  return new TcpConn(stfd);

failed:
  if (stfd) {
    // we must ensure the close is ok.
    assert(st_netfd_close(stfd) != -1);
    stfd = NULL;
  }
  freeaddrinfo(result);
  return NULL;
}