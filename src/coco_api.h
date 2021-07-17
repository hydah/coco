#pragma once
#include <stdint.h>

class UdpConn;
class UdpListener;
class TcpListener;
class TcpConn;

UdpListener *ListenUdp(std::string local_ip, int local_port);
UdpConn *DialUdp(std::string dst_ip, int dst_port, int timeout);

TcpListener *ListenTcp(std::string local_ip, int local_port);
TcpConn *DialTcp(std::string dst_ip, int dst_port, int timeout);

int CocoInit();
int CocoGetCoroutineID();
void CocoLoopMs(uint64_t dur);
void CocoSleepMs(uint64_t durms);
void CocoSleep(uint32_t durs);