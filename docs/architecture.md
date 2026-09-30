# 架构

coco 是基于 State Threads 的 C++11 网络库，接口写成同步调用，阻塞发生在 ST 的读写上，由协程让出。代码来自 SRS 的网络层，范围是 TCP、UDP、TLS、HTTP/1.1 和 WebSocket。

源码按协议分层组织：每一层一个目录、一个静态库，只能依赖它下面的层。想看哪个协议，就打开哪个目录。

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
├── coco_api.h                 ListenTcp / DialTcp / ListenUdp / DialUdp、CocoInit、CocoSleepMs、CocoShouldStop
├── base/                      协程：CoCoroutine、ListenRoutine、ConnRoutine、ConnManager
├── common/error.hpp           错误码
├── log/
├── utils/                     IoReader / IoWriter、FastBuffer、地址、base64/sha1/md5
├── net/
│   ├── coco_socket.hpp/.cpp   st_read / st_write 的超时和错误码
│   ├── layer4/                传输层：TCP、UDP，只有 Conn / Listener / Dial
│   ├── tls/                   安全层：SslConn、SslServer、SslClient
│   └── layer7/                应用层，每个协议一个目录
│       ├── http/              编解码 http_*.h（报文、解析、HttpServeMux）；会话 coco_http（ServeHttpConn、HttpClient）
│       └── ws/                编解码 ws_frame（帧头、WebSocketFrameDecoder）；会话 coco_ws（WebSocketConn、WebSocketClient）
└── server/                    服务运行框架：TcpServer，以及用它组装的 HttpServer
```

每个协议目录里分两种文件。编解码只做字节和报文之间的转换，不碰连接；会话在一条 `StreamConn` 上驱动编解码，负责读写和状态。

`StreamConn` / `DatagramConn` 在 `net/layer4/coco_layer4.hpp`，`TcpConn` 在 `coco_tcp.hpp`，`SslConn` 在 `net/tls/coco_ssl.hpp`。协程 ID 放在 `base/coroutine.hpp` 的 `CoroutineContext` 里。

## 分层

```text
server     TcpServer、HttpServer        组装：accept 循环 + 可选 TLS + 协议处理函数
layer7     HTTP、WebSocket              只认 StreamConn，不知道下面是 TCP 还是 TLS
tls        SslConn，内存 BIO            把一个 StreamConn 包成另一个 StreamConn
layer4     TcpConn、UdpConn、CocoSocket  st_read / st_write / st_accept
core       协程、日志、错误码、工具      st_thread_create
```

规则只有一条：一个文件只能 include 同层或更低层的头文件。`layer7` 下的协议之间默认也互不依赖，目前唯一的例外是 `ws` 可以用 `http`，因为 WebSocket 通过 HTTP Upgrade 建立。

这条规则由 ctest 里的 `LayerDependencies` 用例检查。它运行 `cmake/check_layers.cmake`，扫描 `src/` 下每一条 `#include "..."`，发现向上依赖就列出违规的文件并失败。层号和 `layer7` 内允许的依赖都写在这个脚本开头。

构建上每层一个静态库，`target_link_libraries` 只链接允许依赖的下层：

| 库 | 目录 | 链接 |
| --- | --- | --- |
| `coco_core` | `base/`、`common/`、`log/`、`utils/` | `st`、`pthread` |
| `coco_l4` | `net/coco_socket.*`、`net/layer4/` | `coco_core` |
| `coco_tls` | `net/tls/` | `coco_l4`、`ssl`、`crypto` |
| `coco_l7` | `net/layer7/*/`、http-parser | `coco_tls` |
| `coco_server` | `server/` | `coco_l7` |
| `coco` | 聚合目标（INTERFACE） | `coco_server`，即全部 |

`base/`、`log/`、`utils/` 互相引用（日志要取协程 ID，协程要打日志），所以合成一层 `coco_core`。只写 TCP 程序时可以只链接 `coco_l4`。

一条 TCP 连接的读路径是 `TcpConn::Read` → `CocoSocket::Read` → `st_read`。TLS 连接的明文读路径是 `SslConn::Read` → `SSL_read`，缺密文时再 `st_read` 喂给 `bio_in`。

I/O 接口在 `src/utils/utils.hpp`：`IoReader`、`IoWriter`、`IoReaderWriter`。HTTP 解析用 `FastBuffer` 攒字节。WebSocket 组包有 4MB 上限（`MAX_WS_PACKET`）。

## 层间接口

`layer7` 的服务端入口是一个函数，参数是一条已经建立好的 `StreamConn`，例如 `ServeHttpConn(StreamConn &conn, HttpServeMux *mux)`。它不知道连接是怎么来的：可以是 TCP，可以是握手完成的 TLS，也可以是测试里的内存管道。TLS 同样只面对 `StreamConn`，输入一条，输出一条。

因此在任意两层之间插一层包装，就能观察或改变经过的字节，而不需要改协议代码：打印字节相当于在层间抓包，注入延迟或截断可以做故障实验。

## 并发模型

一个进程里一份 ST，跑在调用 `CocoInit()` 的那条线程上。协程与线程是 1:N：多条协程，一个内核线程。默认栈 64KB。Linux 用 epoll，macOS 用 kqueue。

服务端的结构由 `TcpServer` 固定下来：

1. 一条监听协程（`ListenRoutine`）循环 `Accept()`。`Accept` 持续失败时（例如 `EMFILE`）睡 10ms 再试，不会空转。
2. 每个新连接一条连接协程（`ConnRoutine`）。配置了证书时，先在这条协程上包 `SslServer` 并握手，再调用处理函数，之后的读写只在这条协程里。
3. 处理函数返回后，连接在自己的协程里释放自己，并从 `ConnManager` 的名单里移除。`TcpServer::Stop()` 和析构函数先停监听协程，再中断所有连接并等它们退出。

业务只写处理函数：

```cpp
TcpServer server([](StreamConn &conn) {
    char buf[1024];
    ssize_t n = 0;
    int ret;
    while ((ret = conn.Read(buf, sizeof(buf), &n)) == COCO_SUCCESS) {
        if ((ret = conn.Write(buf, n, nullptr)) != COCO_SUCCESS) break;
    }
    return ret;
});
server.ListenAndServe("127.0.0.1", 8080);
```

处理函数里阻塞的读写和 `CocoSleepMs` 在 `Stop()` 时会返回错误，循环自然结束。不做 I/O 的循环用 `CocoShouldStop()` 判断是否该退出。每条连接的状态直接放在处理函数的局部变量里，或者在处理函数里构造一个会话对象。

需要自定义 accept 策略（限流、按地址拒绝）时，仍可以直接继承 `ListenRoutine` 和 `ConnRoutine`。`examples/pingpong/pingpong_server_tcp.cpp` 用 `TcpServer`，`pingpong_server_udp.cpp` 直接用 `ListenRoutine`。

## 协议

- **TCP / UDP**：`ListenTcp`、`DialTcp`、`ListenUdp`、`DialUdp`。
- **TLS**：服务端和客户端都有。握手不绑定 TLS 1.2 的报文轮次，1.2 和 1.3 都能完成。证书校验是 `SSL_VERIFY_NONE`。
- **HTTP/1.1**：`ServeHttpConn` 按 `HttpServeMux` 派发，支持 keep-alive 和 chunked。`HttpServer` 是 `TcpServer` 加 `ServeHttpConn`，`ListenAndServe` 返回时已经开始服务；HTTPS 由 `TcpServer` 在调用 `ServeHttpConn` 之前完成握手。`HttpClient` 能发 GET/POST，HTTPS 时先做 `SslClient` 握手。
- **WebSocket**：客户端在 `src/net/layer7/ws/coco_ws.cpp`，握手用 HTTP 升级。收到 PING 回 PONG，收到 CLOSE 回一个带相同状态码的 CLOSE 后断开；发送的每一帧用随机掩码，整帧一次写出，并用锁串行化，所以其他协程调用 `Send` 时不会和 PONG 交错。帧的编解码在 `ws_frame.cpp`：`WebSocketFrameDecoder` 自己缓存不完整的帧，把分片拼成完整消息，分片之间插入的控制帧单独交出；违反 RFC 6455 的帧（保留位或 opcode、分片或超过 125 字节的控制帧、单帧或消息超过 `MAX_WS_PACKET`）会让连接回 1002 / 1009 后关闭。

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
| 4051–4053 | WebSocket | `ERROR_WS_PROTOCOL` 4051，`ERROR_WS_MESSAGE_TOO_LARGE` 4052 |

文件里还有一批从 SRS 留下的系统错误码（pid 文件、带宽限制等），当前网络路径不会返回它们。

## 示例与测试

`tests/` 下是 ctest 用例，`./build.sh -t` 会跑它们。`coroutine_test.cpp` 覆盖协程和 `ConnManager` 的生命周期；`tcp_server_test.cpp` 覆盖 `TcpServer` 的回显、关停、处理函数返回、TLS 和 `CocoShouldStop()`；`ws_test.cpp` 覆盖帧的编解码（任意切分、分片与控制帧交错、非法帧）、客户端对 PING / CLOSE 的回复，以及读协程退出时仍有协程阻塞在 `Send` 里的情况；`lifecycle_test.cpp` 通过 `HttpServer`、`WebSocketClient` 走一遍关停和对端关闭的路径；`LayerDependencies` 检查分层。`examples/` 里的程序（TCP/UDP echo、HTTPS 服务端和客户端、WebSocket 客户端）用来手动验证。
