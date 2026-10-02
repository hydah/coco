#include "coco/net/layer4/coco_tcp.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include "st.h"

#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/log/log.hpp"
#include "coco/utils/utils.hpp"

namespace coco {

TcpConn::TcpConn(st_netfd_t stfd) : skt_(stfd) {}

int TcpConn::Read(void *buf, size_t size, ssize_t *nread) { return skt_.Read(buf, size, nread); }

int TcpConn::ReadFully(void *buf, size_t size, ssize_t *nread) {
    return skt_.ReadFully(buf, size, nread);
}

int TcpConn::Write(void *buf, size_t size, ssize_t *nwrite) {
    return skt_.Write(buf, size, nwrite);
}

int TcpConn::Writev(const iovec *iov, int iov_size, ssize_t *nwrite) {
    return skt_.Writev(iov, iov_size, nwrite);
}

std::string TcpConn::LocalAddr() {
    return skt_.get_osfd() < 0 ? std::string() : GetLocalAddr(skt_.get_osfd());
}

std::string TcpConn::RemoteAddr() {
    return skt_.get_osfd() < 0 ? std::string() : GetRemoteAddr(skt_.get_osfd());
}

void TcpConn::SetRecvTimeout(int64_t timeout_us) { skt_.set_recv_timeout(timeout_us); }

void TcpConn::SetSendTimeout(int64_t timeout_us) { skt_.set_send_timeout(timeout_us); }

int TcpConn::CloseWrite() {
    if (skt_.get_osfd() < 0) {
        return ERROR_SOCKET_CLOSED;
    }
    if (shutdown(skt_.get_osfd(), SHUT_WR) == -1) {
        coco_error("shutdown write error. fd=%d, errno=%d", skt_.get_osfd(), errno);
        return ERROR_SOCKET_CLOSED;
    }
    return COCO_SUCCESS;
}

int TcpConn::Release(int *fd) {
    int osfd = skt_.release();
    if (osfd < 0) {
        return ERROR_SOCKET_CLOSED;
    }
    *fd = osfd;
    return COCO_SUCCESS;
}

int TcpConnFromFd(int fd, std::unique_ptr<TcpConn> *conn) {
    if (fd < 0) {
        return ERROR_ST_OPEN_SOCKET;
    }
    int ret = CocoInit();
    if (ret != COCO_SUCCESS) {
        close(fd);
        return ret;
    }
    st_netfd_t stfd = st_netfd_open_socket(fd);
    if (stfd == nullptr) {
        coco_error("open socket fd failed. fd=%d, errno=%d", fd, errno);
        close(fd);
        return ERROR_ST_OPEN_SOCKET;
    }
    conn->reset(new TcpConn(stfd));
    return COCO_SUCCESS;
}

TcpListener::TcpListener(st_netfd_t stfd) : skt_(stfd) {}

int TcpListener::Accept(std::unique_ptr<StreamConn> *conn) {
    std::unique_ptr<TcpConn> tcp;
    int ret = AcceptTcp(&tcp);
    if (ret == COCO_SUCCESS) {
        conn->reset(tcp.release());
    }
    return ret;
}

int TcpListener::AcceptTcp(std::unique_ptr<TcpConn> *conn) {
    st_netfd_t client_stfd =
        st_accept(skt_.get_stfd(), NULL, NULL, (st_utime_t)skt_.get_recv_timeout());
    if (client_stfd == NULL) {
        if (errno == EINTR) {
            return ERROR_THREAD_INTERRUPED;
        }
        if (errno == ETIME) {
            return ERROR_SOCKET_TIMEOUT;
        }
        coco_error("accept client error. errno=%d", errno);
        return ERROR_SOCKET_ACCEPT;
    }
    conn->reset(new TcpConn(client_stfd));
    coco_trace("get a client. fd=%d, remote addr: %s", st_netfd_fileno(client_stfd),
               (*conn)->RemoteAddr().c_str());
    return COCO_SUCCESS;
}

std::string TcpListener::Addr() { return GetLocalAddr(skt_.get_osfd()); }

int ListenTcp(const std::string &ip, int port, std::unique_ptr<TcpListener> *l) {
    st_netfd_t stfd = nullptr;
    int ret = ListenSocket(ip, port, SOCK_STREAM, &stfd);
    if (ret == COCO_SUCCESS) {
        l->reset(new TcpListener(stfd));
    }
    return ret;
}

int DialTcp(const std::string &host, int port, int64_t timeout_us,
            std::unique_ptr<TcpConn> *conn) {
    st_netfd_t stfd = nullptr;
    int ret = DialStream(host, port, timeout_us, &stfd);
    if (ret == COCO_SUCCESS) {
        conn->reset(new TcpConn(stfd));
    }
    return ret;
}

StreamDialer TcpDialer() {
    return [](const std::string &host, int port, int64_t timeout_us,
              std::unique_ptr<StreamConn> *conn) {
        std::unique_ptr<TcpConn> tcp;
        int ret = DialTcp(host, port, timeout_us, &tcp);
        if (ret == COCO_SUCCESS) {
            conn->reset(tcp.release());
        }
        return ret;
    };
}

}  // namespace coco
