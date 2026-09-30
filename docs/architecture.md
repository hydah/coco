# 架构

coco 是基于 State Threads 的 C++11 网络库，接口写成同步调用，阻塞发生在 ST 的读写上，由协程让出。代码来自 SRS 的网络层，范围是 TCP、UDP、TLS、HTTP/1.1 和 WebSocket。

协程调度和连接回收见 [协程与连接管理](coroutine.md)。TLS 记录如何进出协程套接字见 [TLS 握手与读写](tls.md)。

## 依赖

| 依赖 | 位置 | 用途 |
| --- | --- | --- |
| State Threads | submodule `thirdparty/st` | 协程、epoll/kqueue |
| http-parser | submodule `thirdparty/http-parser` | HTTP/1.1 报文解析 |
| OpenSSL 3.5 | 构建时从源码编译，见 `cmake/openssl.cmake` | TLS |

## 源码布局

```text
src/
├── coco_api.h                 ListenTcp / DialTcp / ListenUdp / DialUdp、CocoInit、CocoSleepMs
├── base/
│   ├── coroutine.hpp/.cpp     CoCoroutine、ListenRoutine、ConnRoutine
│   └── coroutine_mgr.hpp/.cpp ConnManager
├── net/
│   ├── coco_socket.hpp/.cpp   st_read / st_write 的超时和错误码
│   ├── layer4/                TCP、UDP、TLS
│   └── layer7/                HTTP 服务端/客户端、WebSocket 客户端
├── protocol/http/             报文、解析、HttpServeMux
├── common/error.hpp           错误码
├── log/
└── utils/                     FastBuffer、地址、base64/sha1/md5
```

实际的类型名字和文件是 `coco_layer4.hpp` 里的 `StreamConn` / `DatagramConn`，`coco_tcp.hpp` 里的 `TcpConn`，`coco_ssl.hpp` 里的 `SslConn`。没有单独的 `layer4_conn` 或 `stream_conn` 文件，协程 ID 也放在 `coroutine.hpp` 的 `CoroutineContext` 里。

## 分层

```text
HTTP / WebSocket          src/net/layer7、src/protocol/http
TLS                       SslConn，内存 BIO，下层仍是 TcpConn
TCP / UDP                 TcpConn、UdpConn
套接字                    CocoSocket → st_read / st_write / st_accept
协程                      CoCoroutine → st_thread_create
```

一条 TCP 连接的读路径是 `TcpConn::Read` → `CocoSocket::Read` → `st_read`。TLS 连接的明文读路径是 `SslConn::Read` → `SSL_read`，缺密文时再 `st_read` 喂给 `bio_in`。

I/O 接口在 `src/utils/utils.hpp`：`IoReader`、`IoWriter`、`IoReaderWriter`。HTTP 解析用 `FastBuffer` 攒字节。WebSocket 组包有 4MB 上限（`MAX_WS_PACKET`）。

## 并发模型

一个进程里一份 ST，跑在调用 `CocoInit()` 的那条线程上。协程与线程是 1:N：多条协程，一个内核线程。默认栈 64KB。Linux 用 epoll，macOS 用 kqueue。

服务端的固定结构：

1. `ListenRoutine` 的协程里循环 `Accept()`。
2. 每个新连接 `new` 一个 `ConnRoutine` 并 `Start()`，之后的读写只在这条连接的协程里。
3. `DoCycle()` 返回后基类 `Remove(this)`，由 `ConnManager` 的清理协程 `delete`。

业务侧继承 `ListenRoutine` 或 `ConnRoutine`，而不是直接继承 `CoCoroutine`。`examples/pingpong/` 和 `examples/http-server/` 是这个结构的两个实例。

## 协议

- **TCP / UDP**：`ListenTcp`、`DialTcp`、`ListenUdp`、`DialUdp`。
- **TLS**：服务端和客户端都有。握手不绑定 TLS 1.2 的报文轮次，1.2 和 1.3 都能完成。证书校验是 `SSL_VERIFY_NONE`。
- **HTTP/1.1**：`HttpServer` 按 `HttpServeMux` 派发，支持 keep-alive 和 chunked。`HttpClient` 能发 GET/POST。HTTPS 是在同一套连接上先做 `SslServer` / `SslClient` 握手。
- **WebSocket**：客户端在 `src/net/layer7/coco_ws.cpp`，握手用 HTTP 升级，帧解析有分片重组。

库里没有连接池，也没有 HTTP/2。一条连接对应一个 `ConnRoutine`，用完即回收。

## 错误码

返回值是 `int`，`COCO_SUCCESS` 为 0。调用方比较返回值，库不使用 C++ 异常。定义在 `src/common/error.hpp`。和当前代码路径相关的主要是：

| 范围 | 含义 | 例子 |
| --- | --- | --- |
| 1000–1012 | 套接字 | `ERROR_SOCKET_TIMEOUT` 1011，`ERROR_SOCKET_CLOSED` 1004 |
| 1013–1018 | ST 初始化、建协程、连接 | `ERROR_ST_CONNECT` 1018 |
| 1070 附近 | 协程停止 | `ERROR_THREAD_INTERRUPED` 1070 |
| 3007–3011 | HTTP 解析和路由 | `ERROR_HTTP_PARSE_HEADER` 3009 |
| 4041–4045 | TLS | `ERROR_HTTPS_HANDSHAKE` 4042 |

文件里还有一批从 SRS 留下的系统错误码（pid 文件、带宽限制等），当前网络路径不会返回它们。

## 示例与测试

仓库没有单元测试。`./build.sh -t` 只会打印还没有配置测试。回归靠 `examples/` 里的程序：TCP/UDP echo、HTTPS 服务端和客户端、WebSocket 客户端。
