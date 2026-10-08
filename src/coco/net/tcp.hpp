#pragma once

#include <memory>
#include <string>

#include "coco/net/socket.hpp"
#include "coco/net/conn.hpp"

namespace coco {

class TcpConn : public StreamConn {
 public:
    // Takes ownership of stfd.
    explicit TcpConn(st_netfd_t stfd);
    virtual ~TcpConn() = default;

    int Read(void *buf, size_t size, ssize_t *nread) override;
    int ReadFully(void *buf, size_t size, ssize_t *nread) override;
    int Write(void *buf, size_t size, ssize_t *nwrite) override;
    int Writev(const iovec *iov, int iov_size, ssize_t *nwrite) override;
    std::string LocalAddr() override;
    std::string RemoteAddr() override;
    void SetRecvTimeout(int64_t timeout_us) override;
    void SetSendTimeout(int64_t timeout_us) override;

    // Shuts down the sending side: the peer reads EOF, while reads here go on.
    int CloseWrite();

    // Gives up the socket without closing it, so that another thread can take it with
    // TcpConnFromFd. No coroutine may be blocked on this connection. Afterwards every call
    // fails with ERROR_SOCKET_CLOSED, and the fd is the caller's to close.
    int Release(int *fd);

 private:
    CocoSocket skt_;
};

// Dials with DialTcp.
StreamDialer TcpDialer();

class TcpListener : public StreamListener {
 public:
    // Takes ownership of stfd, a listening socket.
    explicit TcpListener(st_netfd_t stfd);
    virtual ~TcpListener() = default;

    int Accept(std::unique_ptr<StreamConn> *conn) override;
    int AcceptTcp(std::unique_ptr<TcpConn> *conn);
    std::string Addr() override;

 private:
    CocoSocket skt_;
};

}  // namespace coco
