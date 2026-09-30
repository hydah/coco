#pragma once

#include <sys/socket.h>

#include <functional>
#include <memory>
#include <string>

#include "utils/utils.hpp"

// A byte stream: TCP, TLS over any StreamConn, or anything else that reads and writes in
// order. Write and Writev send every byte or fail. One coroutine may read while others
// write.
class StreamConn : public IoReaderWriter {
 public:
    StreamConn() = default;
    virtual ~StreamConn() = default;

    StreamConn(const StreamConn &) = delete;
    StreamConn &operator=(const StreamConn &) = delete;

    // Reads until size bytes arrived; *nread gets how many did, also on failure.
    virtual int ReadFully(void *buf, size_t size, ssize_t *nread);
    virtual std::string LocalAddr() = 0;
    virtual std::string RemoteAddr() = 0;
    // Bound every later read or write, including those a TLS handshake does.
    virtual void SetRecvTimeout(int64_t timeout_us) = 0;
    virtual void SetSendTimeout(int64_t timeout_us) = 0;
    void SetTimeout(int64_t timeout_us) {
        SetRecvTimeout(timeout_us);
        SetSendTimeout(timeout_us);
    }
};

// Produces StreamConns, e.g. TcpListener, or TlsListener around another listener.
class StreamListener {
 public:
    StreamListener() = default;
    virtual ~StreamListener() = default;

    StreamListener(const StreamListener &) = delete;
    StreamListener &operator=(const StreamListener &) = delete;

    // Blocks until a peer connects; on success *conn owns the new connection.
    virtual int Accept(std::unique_ptr<StreamConn> *conn) = 0;
    virtual std::string Addr() = 0;
};

// Opens a StreamConn to host:port and returns an error code; on success *conn owns it.
// timeout_us bounds connecting, and any handshake the dialer does before returning.
// Clients take one to stay independent of how the stream is made: TcpDialer(), or
// TlsDialer() on top of another dialer.
typedef std::function<int(const std::string &host, int port, int64_t timeout_us,
                          std::unique_ptr<StreamConn> *conn)>
    StreamDialer;

// Datagrams to and from any peer.
class DatagramConn {
 public:
    DatagramConn() = default;
    virtual ~DatagramConn() = default;

    DatagramConn(const DatagramConn &) = delete;
    DatagramConn &operator=(const DatagramConn &) = delete;

    virtual int RecvFrom(void *buf, int size, ssize_t *nread, struct sockaddr *from,
                         int *fromlen) = 0;
    virtual int SendTo(void *buf, int size, ssize_t *nwrite, struct sockaddr *to, int tolen) = 0;
    virtual std::string LocalAddr() = 0;
    virtual void SetRecvTimeout(int64_t timeout_us) = 0;
    virtual void SetSendTimeout(int64_t timeout_us) = 0;
    void SetTimeout(int64_t timeout_us) {
        SetRecvTimeout(timeout_us);
        SetSendTimeout(timeout_us);
    }
};
