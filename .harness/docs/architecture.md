# 架构

coco 是基于 State Threads 的 C++11 网络库，接口写成同步调用，阻塞发生在 ST 的读写上，由协程让出。支持的协议是 TCP、UDP、TLS、HTTP/1.1、WebSocket 和 RTMP。

源码按协议分层组织：每一层一个目录，只能依赖它下面的层。想看哪个协议，就打开哪个目录。对外只有一个库 `libcoco`，所有名字在 `namespace coco` 里。

协程调度和连接回收见 [协程与连接管理](coroutine.md)。TLS 记录如何进出协程套接字见 [TLS 握手与读写](tls.md)。

## 依赖

| 依赖 | 位置 | 用途 |
| --- | --- | --- |
| State Threads | submodule `thirdparty/st` | 协程、epoll/kqueue |
| http-parser | submodule `thirdparty/http-parser` | HTTP/1.1 报文解析 |
| OpenSSL 3.5 | 构建时从源码编译，见 `cmake/openssl.cmake` | TLS |

## 源码布局

代码都在 `src/coco/` 下，include 一律写 `"coco/..."`；安装时这个目录原样装到 `include/coco/`，只去掉库内部用的头文件（`utils/utils.hpp`、`md5` / `sha1` / `base64`、`base/shutdown.hpp`）。

```text
src/coco/
├── coco.h                     汇总头文件
├── coco_api.h                 ListenTcp / DialTcp / ListenUdp / DialUdp、CocoInit、CocoRun、CocoWaitForShutdown / CocoShutdown、CocoSleepMs、CocoShouldStop
├── base/                      协程：CoCoroutine、ListenRoutine、ConnRoutine、ConnManager；st_fwd.hpp；shutdown（退出请求和信号）
├── common/error.hpp           错误码
├── log/
├── utils/                     io.hpp（IoReader / IoWriter）、BufReader；内部：地址、base64/sha1/md5
├── net/
│   ├── coco_socket.hpp/.cpp   拥有 st_netfd 的 CocoSocket：超时和错误码；建 socket、bind、connect
│   ├── layer4/                传输层：StreamConn / StreamListener / DatagramConn 接口，TCP、UDP 实现
│   ├── tls/                   安全层：TlsConfig、TlsConn、TlsListener、TlsDialer
│   └── layer7/                应用层，每个协议一个目录
│       ├── http/              编解码 http_*.h（头部与工具、报文与 body、HttpResponseWriter、HttpServeMux）；会话 coco_http（ServeHttpConn、HttpClient）
│       ├── ws/                编解码 ws_frame（帧头、WebSocketFrameDecoder）；会话 coco_ws（WebSocketConn、WebSocketClient、WebSocketHandler）
│       └── rtmp/              握手、chunk、AMF0；会话 coco_rtmp（RtmpConn、RtmpClient、ServeRtmpConn）
└── server/                    服务运行框架：TcpServer，以及用它组装的 HttpServer、RtmpServer
```

每个协议目录里分两种文件。编解码只做字节和报文之间的转换，不碰连接；会话在一条 `StreamConn` 上驱动编解码，负责读写和状态。

`StreamConn` / `StreamListener` / `DatagramConn` 在 `net/layer4/coco_layer4.hpp`，都是纯接口，不含 fd。`TcpConn` / `TcpListener` 在 `coco_tcp.hpp`，`UdpConn` / `UdpListener` 在 `coco_udp.hpp`，它们各自持有一个 `CocoSocket`，析构时关闭 fd。`TlsConn` 在 `net/tls/coco_tls.hpp`，它拥有一条下层 `StreamConn`，自己不碰 fd。协程 ID 放在 `base/coroutine.hpp` 的 `CoroutineContext` 里。

`ListenTcp`、`DialTcp`、`ListenUdp`、`DialUdp` 返回错误码，成功时通过 `std::unique_ptr` 出参交出新连接。`DialTcp` 依次尝试解析出的每个地址。`StreamDialer` 是「给 host:port 建一条 `StreamConn`」的函数类型，`TcpDialer()` 用 `DialTcp` 实现它，`TlsDialer()` 在另一个 dialer 之上做 TLS 握手。

## 分层

```text
server     TcpServer、HttpServer、RtmpServer   组装：accept 循环 + 可选 TLS + 协议处理函数
layer7  |  HTTP、WebSocket、RTMP               只认 StreamConn 和 StreamDialer，不知道下面是 TCP 还是 TLS
tls     |  TlsConn、TlsListener、TlsDialer      把一个 StreamConn 包成另一个 StreamConn
layer4     StreamConn 等接口；TcpConn、UdpConn  st_read / st_write / st_accept
core       协程、日志、错误码、工具             st_thread_create
```

`layer7` 和 `tls` 平级，互不依赖，都只依赖 `layer4`，由 `server` 或调用方组合。HTTP 和 WebSocket 的客户端不自己建连，而是调用注入的 `StreamDialer`（`layer4` 里的函数类型）：默认 `TcpDialer()`，https / wss 传 `TlsDialer()`。所以 `layer7` 的代码里没有 OpenSSL，`tls` 也可以套在任何能产出 `StreamConn` 的东西上。

规则：一个文件只能 include 同一层或层号更低的头文件；层号相同但层名不同的平级层（`layer7` 和 `tls`）互相不能 include。`layer7` 下的协议之间默认也互不依赖，目前唯一的例外是 `ws` 可以用 `http`，因为 WebSocket 通过 HTTP Upgrade 建立。

这些规则由 ctest 里的 `LayerDependencies` 用例检查。它运行 `cmake/check_layers.cmake`，扫描 `src/` 下每一条 `#include "..."`，发现向上或跨平级层的依赖就列出违规的文件并失败。层号和 `layer7` 内允许的依赖都写在这个脚本开头。

各层对应的目录：

| 层 | 目录 |
| --- | --- |
| core | `base/`、`common/`、`log/`、`utils/` |
| l4 | `net/coco_socket.*`、`net/layer4/` |
| tls | `net/tls/` |
| l7 | `net/layer7/*/`，加上 http-parser |
| server | `server/` |

`base/`、`log/`、`utils/` 互相引用（日志要取协程 ID，协程要打日志），所以合成一层 core。

构建上所有层编进同一个库 `libcoco`（CMake 目标 `coco::coco`），分层只靠上面的 include 规则保证，不再拆成每层一个库：拆开的好处是只用明文协议时可以不链接 OpenSSL，但代价是使用方要自己按顺序列出五个库再加上 ST 和 OpenSSL。ST、OpenSSL、http-parser 都是 `PRIVATE` 依赖：公共头文件里只有它们句柄类型的前置声明（`base/st_fwd.hpp`、`net/tls/coco_tls.hpp` 开头的 `SSL` / `SSL_CTX` / `BIO`），不 include 它们的头文件。

一条 TCP 连接的读路径是 `TcpConn::Read` → `CocoSocket::Read` → `st_read`。TLS 连接的明文读路径是 `TlsConn::Read` → `SSL_read`，缺密文时再调用下层的 `Read`（TCP 时就是上面那条路径）喂给 `bio_in`。

I/O 接口在 `src/coco/utils/io.hpp`：`IoReader`、`IoWriter`、`IoReaderWriter`。`BufReader`（`src/coco/utils/bufio.hpp`）是读缓冲，仿照 Go 的 `bufio.Reader`：协议解析在缓冲区里看字节、消费用掉的部分，剩下的留给同一条连接上的下一条报文。WebSocket 组包有 4MB 上限（`MAX_WS_PACKET`）。

## 层间接口

`layer7` 的服务端入口是一个函数，参数是一条已经建立好的 `StreamConn`，例如 `ServeHttpConn(StreamConn &conn, HttpHandler *handler)`。它不知道连接是怎么来的：可以是 TCP，可以是握手完成的 TLS，也可以是测试里的内存管道。TLS 同样只面对 `StreamConn`，输入一条，输出一条。

因此在任意两层之间插一层包装，就能观察或改变经过的字节，而不需要改协议代码：打印字节相当于在层间抓包，注入延迟或截断可以做故障实验。

## 并发模型

一个进程里一份 ST，跑在初始化它的那条线程上：显式调用 `CocoInit()`，或者第一次建协程、建 socket、`CocoSleepMs` 时自动初始化。协程与线程是 1:N：多条协程，一个内核线程。默认栈 64KB。Linux 用 epoll，macOS 用 kqueue。

服务端的结构由 `TcpServer` 固定下来：

1. 一条监听协程（`ListenRoutine`）循环调用 `StreamListener::Accept()`。`Accept` 持续失败时（例如 `EMFILE`）睡 10ms 再试，不会空转。`TcpServer::Start` / `Serve` 接受任何 `StreamListener`；配置了证书时，它把监听器包成 `TlsListener`。
2. 每个新连接一条连接协程（`ConnRoutine`）。TLS 握手推迟到处理函数第一次读写，所以也在这条协程上，之后的读写只在这条协程里。
3. 处理函数返回后，连接在自己的协程里释放自己，并从 `ConnManager` 的名单里移除。`TcpServer::Stop()` 和析构函数先停监听协程，再关闭监听 socket（新连接立刻被拒绝，端口马上可以重用），然后中断所有连接并等它们退出。两个协程同时调用 `Stop()` 时，后到的等先到的停完再返回，所以任何一个 `Stop()` 返回时服务都已经完全停下。
4. `ListenAndServe` / `Serve` 是 `Start` 加 `Wait()`：调用它的协程（通常是主协程）停在一个条件变量上，直到 `Stop()` 或退出请求。退出请求来自 `CocoShutdown()` 或 `SIGINT` / `SIGTERM`：信号处理函数只往一个 pipe 里写一个字节，由一条协程读出来再调用 `CocoShutdown()`，所以真正的关停逻辑都跑在普通协程上，不在信号上下文里。信号在第一次等待（或 `CocoRun`）时才接管：第一个信号请求退出，并把信号还给原来的处理方式；第二个信号直接按默认动作结束进程。这个计数放在信号处理函数里，所以即使没有协程能运行（某段代码一直不让出）也有效；启动时就被忽略的信号保持忽略。
5. `CocoRun(fn)` 在主协程上直接调用 `fn`（用进程自己的栈，不是 64KB 的协程栈），并在它运行期间把退出请求变成对这条协程的一次中断加上 `CocoShouldStop()` 为 true：正在阻塞的调用失败一次，循环自己退出，`fn` 栈上的对象照常析构。

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
server.ListenAndServe("127.0.0.1", 8080);   // 到 Ctrl-C 为止
```

处理函数里阻塞的读写和 `CocoSleepMs` 在 `Stop()` 时会返回错误，循环自然结束。不做 I/O 的循环用 `CocoShouldStop()` 判断是否该退出。每条连接的状态直接放在处理函数的局部变量里，或者在处理函数里构造一个会话对象。

需要自定义 accept 策略（限流、按地址拒绝）时，仍可以直接继承 `ListenRoutine` 和 `ConnRoutine`。`examples/pingpong/pingpong_server_tcp.cpp` 用 `TcpServer`，`pingpong_server_udp.cpp` 直接用 `ListenRoutine`。

## 协议

- **TCP / UDP**：`ListenTcp`、`DialTcp`、`ListenUdp`、`DialUdp`。
- **TLS**：服务端和客户端都有，可以套在任何 `StreamConn` 上。`TlsConfig` 共享 `SSL_CTX`，证书只加载一次。握手不绑定 TLS 1.2 的报文轮次，1.2 和 1.3 都能完成。证书校验是 `SSL_VERIFY_NONE`。
- **HTTP/1.1**：接口仿照 Go 的 `net/http`。
  - 服务端：处理函数是 `HttpHandler::ServeHTTP(HttpResponseWriter &w, HttpRequest &r)`，或者用 `HandleFunc` 注册 lambda。`HttpServeMux` 支持 Go 1.22 的模式语法：可带方法（`"GET /users/{id}"`，GET 也接 HEAD）、主机、`{name}` 单段通配、`{name...}` 尾段、以 `/` 结尾的子树和 `{$}`。路由是按路径段建的树，越具体越优先：字面段优先于 `{name}`，再优先于子树；带方法的优先于不带方法的。和 Go 一样，不带方法的模式接受任何方法；只有路径匹配而所有模式的方法都不匹配时才回 405 和 `Allow`。含 `.`、`..`、`//` 的路径先 301 到规范形式；注册了 `/tree/` 而没有模式精确匹配 `/tree` 时，`/tree` 301 到 `/tree/`。`HttpServer` 是 `TcpServer` 加 `ServeHttpConn`，`ListenAndServe` / `ListenAndServeTLS` 一直服务到 `Stop()` 或退出请求，`Start` / `StartTLS` 开始服务后立即返回；HTTPS 由 `TcpServer` 的 `TlsListener` 提供，`ServeHttpConn` 第一次读请求时完成握手。
  - 连接循环：一条连接在整个生命周期里只用一个 `BufReader`、一个 `HttpResponseWriter` 和一个 `HttpRequest`，缓冲区跨请求复用，流水线请求中提前读到的字节不会丢。请求头只把头部字节交给 http-parser 解析，body 由 `HttpBodyReader` 按 Content-Length 或 chunked 从同一个缓冲区读。处理函数没读完的 body 在 256KB 以内会被跳过以保住 keep-alive，更长就关连接。`Expect: 100-continue` 在处理函数第一次读 body 时才回 100。请求头超限回 431，格式错误或 HTTP/1.1 缺 Host 回 400，之后关连接。
  - 写响应：语义同 Go 的 `ResponseWriter`。写入先进 4KB 缓冲；处理函数返回时还没超过缓冲区、又没设 Content-Length，就自动补上 Content-Length，状态行、头部和 body 一次写出；超过缓冲区或调用 `Flush()` 后改成 chunked（HTTP/1.0 则以关连接结束），大块数据用 `writev` 直接发出，不拷进缓冲区。没设 Content-Type 时按前 512 字节嗅探，自动加 Date。`Hijack()` 交出连接和读缓冲，之后服务端不再碰这条连接，WebSocket 就是这样接管的；Upgrade 请求没被接管时，响应后关连接。
  - 客户端：`HttpClient` 仿照 Go 的 `http.Client`，`Get` / `Post` / `Do(HttpRequest&)` 返回 `std::unique_ptr<HttpResponse>`，从 `resp->body` 读 body。keep-alive 连接按 `scheme://host:port` 放进连接池复用（每个主机默认留 2 条空闲）；`HttpResponse` 析构时，body 已读完（或者剩下的部分已经全在缓冲区里）就把连接还回池子，否则关掉。从池里取出的连接若已被服务端关闭，请求会在新连接上重试一次：写失败时总是重试，读失败时只重试 GET、HEAD、OPTIONS、TRACE。默认跟随最多 10 次重定向，301/302/303 把非 GET/HEAD 改成不带 body 的 GET，跨主机时去掉 Authorization 和 Cookie。连接由注入的 `StreamDialer` 建立，`http://` 默认 `TcpDialer()`，`https://` 需要 `SetTlsDialer(TlsDialer())`。`HttpGet` / `HttpPost` 用一个共享的默认客户端。
- **WebSocket**：在 `src/coco/net/layer7/ws/coco_ws.cpp`。`WebSocketConn` 是握手之后的一条连接，客户端和服务端共用。一条协程用 `ReadMessage()` 同步读下一条数据消息（分片已拼好），PING 和 CLOSE 在读的过程中顺带回复，所以总得有协程在读；对端的 CLOSE 要等它前面的消息都被读走后才回，这样对这些消息的回复能先发出去。`Send` 可以在任意协程调用，整帧一次写出并用锁串行化，所以不会和 PONG 交错。客户端 `WebSocketClient` 用 HTTP 升级握手，发送的每一帧用随机掩码，有两种用法：仿照 Go 的 `Dial("ws://host:port/path")`（`wss://` 要先 `SetTlsDialer(TlsDialer())`，否则返回 `ERROR_HTTPS_NOT_SUPPORTED`）之后由调用方自己循环 `ReadMessage()`，析构时连接还开着就发 CLOSE 1000；或者 `Start()`，另起一条读协程循环读，把消息交给 `SetMessageHandler` 的回调，这时不能再调 `ReadMessage()`。服务端仿照 Go：`WebSocketHandler` 包一个 `void(WebSocketConn *)` 函数，注册到 `HttpServeMux` 上，`HttpServer` 或带 TLS 的 `TcpServer`（wss）都能用。它校验升级请求（`GET`、`Upgrade: websocket`、`Connection: upgrade`、16 字节的 `Sec-WebSocket-Key`、版本 13），不合格回 400；握手成功后在这条 HTTP 连接的协程里调用这个函数，函数就是连接的生命周期，返回时若连接还开着就发 CLOSE 1000，再等还卡在 `Send` 里的协程（最多等到发送超时），然后释放连接。别的协程可以拿着 `WebSocketConn*` 推送，但只能到这个函数返回为止。服务端发送不加掩码，收到不带掩码的帧按 1002 关闭。握手时 `WebSocketHandler` 用 `Hijack()` 接管连接，之后 `ServeHttpConn` 不再按 HTTP 读它。帧的编解码在 `ws_frame.cpp`：`WebSocketFrameDecoder` 自己缓存不完整的帧，把分片拼成完整消息，分片之间插入的控制帧单独交出；违反 RFC 6455 的帧（保留位或 opcode、分片或超过 125 字节的控制帧、单帧或消息超过 `MAX_WS_PACKET`）会让连接回 1002 / 1009 后关闭。
- **RTMP**：在 `src/coco/net/layer7/rtmp/`。握手先认复杂握手（HMAC-SHA256 digest，两种 scheme 都接受），对不上再退回简单握手（版本字段为 0，S2/C2 回显对端的 1536 字节）。`RtmpConn` 在一条 `StreamConn` 上收发已经按 chunk 拼好的消息；chunk size、acknowledgement、peer bandwidth 和 ping 在 `ReadMessage` 里处理，写出整条消息一次完成并用锁串行化，所以应答不会和业务写交错。`ServeRtmpConn` 完成 connect 和 createStream，收到 publish 或 play 后先回 NetStream 状态，再把连接交给处理函数。`RtmpClient` 解析 `rtmp://host[:port]/app/stream`（第一段是 app，其余是流名）；`rtmps://` 要先 `SetDialer(TlsDialer())`。`RtmpServer` 是 `TcpServer` 加 `ServeRtmpConn`，`ListenAndServeTLS` 就是 RTMPS。单条消息受 24 位长度限制（`kRtmpMaxMessage`，16777215 字节）。不实现 RTMPE 和 shared object，aggregate 消息原样交给调用方。

库里没有 HTTP/2。服务端一条连接对应一个 `ConnRoutine`，用完即回收；连接池只在 `HttpClient` 里。

## 错误码

返回值是 `int`，`COCO_SUCCESS` 为 0。调用方比较返回值，库不使用 C++ 异常。定义在 `src/coco/common/error.hpp`，除 `COCO_SUCCESS` 是宏以外都是 `namespace coco` 里的 `constexpr int`。和当前代码路径相关的主要是：

| 范围 | 含义 | 例子 |
| --- | --- | --- |
| 1000–1012 | 套接字 | `ERROR_SOCKET_TIMEOUT` 1011，`ERROR_SOCKET_CLOSED` 1004 |
| 1013–1018 | ST 初始化、建协程、连接 | `ERROR_ST_CONNECT` 1018 |
| 1070 附近 | 协程停止 | `ERROR_THREAD_INTERRUPED` 1070 |
| 3007–3011 | HTTP 解析和路由 | `ERROR_HTTP_PARSE_HEADER` 3009 |
| 4000–4034 | HTTP 会话 | `ERROR_HTTP_BODY_EOF` 4030（body 已读完），`ERROR_HTTP_HEADER_TOO_LARGE` 4031 |
| 4041–4045 | TLS | `ERROR_HTTPS_HANDSHAKE` 4042 |
| 4051–4053 | WebSocket | `ERROR_WS_PROTOCOL` 4051，`ERROR_WS_MESSAGE_TOO_LARGE` 4052 |
| 4061–4065 | RTMP          | `ERROR_RTMP_HANDSHAKE` 4062，`ERROR_RTMP_MESSAGE_TOO_LARGE` 4063 |

文件里还有一批系统错误码（pid 文件、带宽限制等），当前网络路径不会返回它们。

## 示例与测试

`tests/` 下是 ctest 用例，`./build.sh -t` 会跑它们。`coroutine_test.cpp` 覆盖协程和 `ConnManager` 的生命周期；`tcp_server_test.cpp` 覆盖 `TcpServer` 的回显、关停、处理函数返回、TLS 和 `CocoShouldStop()`；`ws_test.cpp` 覆盖帧的编解码（任意切分、分片与控制帧交错、非法帧）、客户端对 PING / CLOSE 的回复、`Dial` 的 URL 解析和析构时的 CLOSE 1000，以及读协程退出时仍有协程阻塞在 `Send` 里的情况；`ws_server_test.cpp` 覆盖服务端的握手（大小写不同的头、紧跟在请求后面的帧）、非法升级回 400、PING / CLOSE（和 CLOSE 同包到达的消息仍会被读到）、拒收不带掩码的帧、处理函数返回时发 CLOSE 1000、关停时结束已打开的连接，以及 wss；`http_test.cpp` 覆盖 HTTP 的响应分帧（自动 Content-Length、chunked、Flush）、流水线、各种请求 body 与未读 body 的跳过、100-continue、HEAD、HTTP/1.0、431/400/417、路由规则和 405、请求字段的解码，以及客户端的连接复用、过期连接重试、重定向和响应分帧；`lifecycle_test.cpp` 通过 `HttpServer`、`WebSocketClient` 走一遍关停和对端关闭的路径；`runtime_test.cpp` 覆盖 `CocoInit()` 的幂等、`CocoShutdown()` 和信号唤醒等待者、阻塞式 `ListenAndServe` 在 `Stop()` / 退出请求 / 处理函数里 `CocoShutdown()` 时返回、`Stop()` 关闭监听端口且并发调用安全，以及 `CocoRun` 的各条退出路径；其中两个用例起子进程发真实信号：第一个 `SIGTERM` 让服务以状态 0 退出，第二个 `SIGINT` 结束一个一直不让出协程的进程；`LayerDependencies` 检查分层。`rtmp_test.cpp` 覆盖 AMF0、chunk（含扩展时间戳和交错）、URL，以及本机推流再拉流。`examples/` 里的程序（TCP/UDP echo、HTTPS 服务端和客户端、WebSocket 客户端和回显服务端、RTMP 直播转发）用来手动验证。
