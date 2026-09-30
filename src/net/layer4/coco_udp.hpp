#pragma once

#include <memory>
#include <string>

#include "net/coco_socket.hpp"
#include "net/layer4/coco_layer4.hpp"

// A datagram socket with a default peer, as returned by DialUdp.
class UdpConn : public DatagramConn {
 public:
    // Takes ownership of stfd.
    UdpConn(st_netfd_t stfd, const sockaddr_storage &peer, socklen_t peer_len);
    virtual ~UdpConn() = default;

    // Receives one datagram from any sender.
    int Read(void *buf, int size, ssize_t *nread);
    // Sends one datagram to the peer.
    int Write(void *buf, int size, ssize_t *nwrite);

    int RecvFrom(void *buf, int size, ssize_t *nread, struct sockaddr *from,
                 int *fromlen) override;
    int SendTo(void *buf, int size, ssize_t *nwrite, struct sockaddr *to, int tolen) override;
    std::string LocalAddr() override;
    void SetRecvTimeout(int64_t timeout_us) override;
    void SetSendTimeout(int64_t timeout_us) override;

 private:
    CocoSocket skt_;
    sockaddr_storage peer_;
    socklen_t peer_len_;
};

// A bound datagram socket, as returned by ListenUdp.
class UdpListener : public DatagramConn {
 public:
    // Takes ownership of stfd.
    explicit UdpListener(st_netfd_t stfd);
    virtual ~UdpListener() = default;

    int RecvFrom(void *buf, int size, ssize_t *nread, struct sockaddr *from,
                 int *fromlen) override;
    int SendTo(void *buf, int size, ssize_t *nwrite, struct sockaddr *to, int tolen) override;
    std::string LocalAddr() override;
    void SetRecvTimeout(int64_t timeout_us) override;
    void SetSendTimeout(int64_t timeout_us) override;

 private:
    CocoSocket skt_;
};
