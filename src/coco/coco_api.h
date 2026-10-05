#pragma once
#include <stdint.h>

#include <functional>
#include <memory>
#include <string>

namespace coco {

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
// Takes ownership of fd, a connected TCP socket, on the calling thread's runtime; it is
// closed if that fails. With TcpConn::Release() it moves a connection to another thread.
int TcpConnFromFd(int fd, std::unique_ptr<TcpConn> *conn);

// Sets up the coroutine runtime of the calling thread; later calls do nothing. A program
// normally starts with CocoRun(), which does this. Calling it is optional, the first
// function that needs the runtime does it, but calling it first reports a failure up
// front. Every thread that uses coco has a runtime of its own, and what coco creates
// belongs to the thread that created it: only a released fd (TcpConn::Release()) and a
// function posted to a CocoThread move between threads.
int CocoInit();
int CocoGetCoroutineID();
void CocoSleepMs(uint64_t durms);
void CocoSleep(uint32_t durs);
// Lets the other coroutines of the thread run, and the thread take in what arrived, a stop
// or shutdown request included, then resumes. Scheduling is cooperative: until the running
// coroutine blocks or yields, nothing else on its thread runs, so a loop that does no I/O
// calls this every few milliseconds, then checks CocoShouldStop(). Work that takes long
// without yielding belongs to a thread of its own, not to one serving I/O.
void CocoYield();
// True once the coroutine running this code has been stopped or interrupted. Lets a
// plain function (e.g. a TcpServer handler) exit a loop that does no blocking I/O. On the
// coroutine CocoRun() runs its function on it turns true with a shutdown request; on any
// other coroutine coco did not make it is always false.
bool CocoShouldStop();

// Blocks the calling coroutine, while all others keep running, until SIGINT or SIGTERM
// arrives or CocoShutdown() is called. Returns the signal, or 0 for CocoShutdown() and
// when the calling coroutine is interrupted.
//
// The first call arms SIGINT and SIGTERM: the first signal requests the shutdown and gives
// the signals back to what handled them before, and a second one ends the process, even
// if shutting down hangs or no coroutine gets to run. A signal that was ignored when
// armed, as for a background job, stays ignored.
int CocoWaitForShutdown();
// Wakes every CocoWaitForShutdown() and every blocking Serve or ListenAndServe, which
// then stop their server, and stops the function of a running CocoRun(), on every thread.
// It may be called from any thread, one without a runtime included. Never blocks, so a
// connection handler may call it; it is not async-signal-safe.
void CocoShutdown();
bool CocoShutdownRequested();

// Runs fn as the body of the program and returns what it returns, or the error when the
// runtime cannot be set up. Besides CocoInit(), it makes the body stoppable: when SIGINT,
// SIGTERM or CocoShutdown() asks for a shutdown, CocoShouldStop() turns true and the
// blocking call fn is in fails, so a loop that reads or sleeps ends by itself and the
// objects on fn's stack are destroyed as usual. A blocking ListenAndServe still stops its
// server and returns COCO_SUCCESS. Only the blocking call in progress fails, later ones
// work, so a loop checks CocoShouldStop().
//
// fn runs on the calling coroutine, the main one, with the stack of the thread. A second
// CocoRun() inside fn, or on another coroutine of the same thread, returns
// ERROR_THREAD_STARTED.
//
//   int main() {
//       return CocoRun([]() {
//           std::unique_ptr<TcpConn> conn;
//           if (DialTcp("127.0.0.1", 8080, 1000 * 1000, &conn) != COCO_SUCCESS) return 1;
//           while (!CocoShouldStop()) {  // false until a shutdown is requested
//               // ... conn->Write(), conn->Read(), CocoSleepMs() ...
//           }
//           return 0;  // conn is closed here
//       });
//   }
int CocoRun(const std::function<int()> &fn);

// Same as CocoWaitForShutdown(); dur is ignored.
__attribute__((deprecated("use CocoWaitForShutdown() or a blocking ListenAndServe")))
void CocoLoopMs(uint64_t dur);

}  // namespace coco
