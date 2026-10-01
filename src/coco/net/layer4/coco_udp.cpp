#include "coco/net/layer4/coco_udp.hpp"

#include <assert.h>
#include <string.h>

#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/log/log.hpp"
#include "coco/utils/utils.hpp"

namespace coco {

UdpConn::UdpConn(st_netfd_t stfd, const sockaddr_storage &peer, socklen_t peer_len)
    : skt_(stfd), peer_len_(peer_len) {
    assert(peer_len <= sizeof(peer_));
    memcpy(&peer_, &peer, peer_len);
}

int UdpConn::Read(void *buf, int size, ssize_t *nread) {
    sockaddr_storage from;
    int fromlen = sizeof(from);
    return skt_.recvfrom(buf, size, nread, (struct sockaddr *)&from, &fromlen);
}

int UdpConn::Write(void *buf, int size, ssize_t *nwrite) {
    return skt_.sendto(buf, size, nwrite, (struct sockaddr *)&peer_, (int)peer_len_);
}

int UdpConn::RecvFrom(void *buf, int size, ssize_t *nread, struct sockaddr *from, int *fromlen) {
    return skt_.recvfrom(buf, size, nread, from, fromlen);
}

int UdpConn::SendTo(void *buf, int size, ssize_t *nwrite, struct sockaddr *to, int tolen) {
    return skt_.sendto(buf, size, nwrite, to, tolen);
}

std::string UdpConn::LocalAddr() { return GetLocalAddr(skt_.get_osfd()); }

void UdpConn::SetRecvTimeout(int64_t timeout_us) { skt_.set_recv_timeout(timeout_us); }

void UdpConn::SetSendTimeout(int64_t timeout_us) { skt_.set_send_timeout(timeout_us); }

UdpListener::UdpListener(st_netfd_t stfd) : skt_(stfd) {}

int UdpListener::RecvFrom(void *buf, int size, ssize_t *nread, struct sockaddr *from,
                          int *fromlen) {
    return skt_.recvfrom(buf, size, nread, from, fromlen);
}

int UdpListener::SendTo(void *buf, int size, ssize_t *nwrite, struct sockaddr *to, int tolen) {
    return skt_.sendto(buf, size, nwrite, to, tolen);
}

std::string UdpListener::LocalAddr() { return GetLocalAddr(skt_.get_osfd()); }

void UdpListener::SetRecvTimeout(int64_t timeout_us) { skt_.set_recv_timeout(timeout_us); }

void UdpListener::SetSendTimeout(int64_t timeout_us) { skt_.set_send_timeout(timeout_us); }

int ListenUdp(const std::string &ip, int port, std::unique_ptr<UdpListener> *l) {
    st_netfd_t stfd = nullptr;
    int ret = ListenSocket(ip, port, SOCK_DGRAM, &stfd);
    if (ret == COCO_SUCCESS) {
        l->reset(new UdpListener(stfd));
    }
    return ret;
}

int DialUdp(const std::string &host, int port, int64_t timeout_us,
            std::unique_ptr<UdpConn> *conn) {
    st_netfd_t stfd = nullptr;
    sockaddr_storage peer;
    socklen_t peer_len = 0;
    int ret = DialDatagram(host, port, &stfd, &peer, &peer_len);
    if (ret == COCO_SUCCESS) {
        conn->reset(new UdpConn(stfd, peer, peer_len));
        (*conn)->SetSendTimeout(timeout_us);
    }
    return ret;
}

}  // namespace coco
