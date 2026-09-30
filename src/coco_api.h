#pragma once
#include <stdint.h>

#include <memory>
#include <string>

class UdpConn;
class UdpListener;
class TcpListener;
class TcpConn;

// Each returns an error code; on success the out parameter owns the new socket.
// local_ip must be an IP literal. timeout_us bounds each connect attempt, or for UDP
// every send.
int ListenUdp(const std::string &local_ip, int local_port, std::unique_ptr<UdpListener> *l);
int DialUdp(const std::string &host, int port, int64_t timeout_us, std::unique_ptr<UdpConn> *conn);

int ListenTcp(const std::string &local_ip, int local_port, std::unique_ptr<TcpListener> *l);
int DialTcp(const std::string &host, int port, int64_t timeout_us, std::unique_ptr<TcpConn> *conn);

int CocoInit();
int CocoGetCoroutineID();
void CocoLoopMs(uint64_t dur);
void CocoSleepMs(uint64_t durms);
void CocoSleep(uint32_t durs);
// True once the coroutine running this code has been stopped or interrupted. Lets a
// plain function (e.g. a TcpServer handler) exit a loop that does no blocking I/O.
bool CocoShouldStop();