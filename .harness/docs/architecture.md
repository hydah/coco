# 架构

coco 是基于 State Threads 的 C++11 网络库，接口写成同步调用，阻塞发生在 ST 的读写上，由协程让出。支持的协议是 TCP、UDP、RUDP（UDP 上的可靠字节流）、TLS、HTTP/1.1、WebSocket 和 RTMP，主机名由 net 里的 DNS 解析器解析。接下来要加的协议见 [协议规划](protocols.md)。

网络代码分成两个目录，划分只看一个问题：它是不是在帮你拿到一条字节流（`StreamConn`）。是的放 `net/`：TCP、UDP、拨号和监听、`TcpServer` 的接受循环，在 UDP 上做出字节流的 `net/rudp/`，以及 `net/dns/`（`DialTcp("host")` 要先解析主机名）和 `net/tls/`（把一条字节流变成加密的另一条，`TlsDialer` 和 `TcpDialer` 是同一类东西）。在字节流上说话的应用层协议放 `app/`：HTTP、WebSocket、RTMP 各一个目录，想看哪个协议，就打开哪个目录，目录里再按 codec、会话、服务器分层。哪个路径属于哪一层、能依赖谁，写在 `cmake/check_layers.cmake` 里。对外只有一个库 `libcoco`，所有名字在 `namespace coco` 里。

协程调度和连接回收见 [协程与连接管理](coroutine.md)。TLS 记录如何进出协程套接字见 [TLS 握手与读写](tls.md)。

## 依赖

| 依赖 | 位置 | 用途 |
| --- | --- | --- |
| State Threads | submodule `thirdparty/st` | 协程、epoll/kqueue |
| http-parser | submodule `thirdparty/http-parser` | HTTP/1.1 报文解析 |
| OpenSSL 3.5 | 构建时从源码编译，见 `cmake/openssl.cmake` | TLS |

## 源码布局

代码都在 `src/coco/` 下，include 一律写 `"coco/..."`；安装时这个目录原样装到 `include/coco/`，只去掉库内部用的头文件（`utils/utils.hpp`、`md5` / `sha1` / `base64`、`base/shutdown.hpp`、`net/rudp/endpoint.hpp`）。

```text
src/coco/
├── coco.h                     汇总头文件
├── coco_api.h                 ListenTcp / DialTcp / TcpConnFromFd / ListenUdp / DialUdp、CocoInit、CocoRun、CocoWaitForShutdown / CocoShutdown、CocoSleepMs、CocoShouldStop
├── base/                      协程：CoCoroutine、ListenRoutine、ConnRoutine、ConnManager；TaskGroup；CocoThread；OwnerThread；st_fwd.hpp；shutdown（跨线程的退出请求和信号）
├── common/error.hpp           错误码
├── log/
├── utils/                     io.hpp（IoReader / IoWriter）、BufReader；内部：地址、base64/sha1/md5
├── net/                       拿到字节流
│   ├── conn.hpp               StreamConn / StreamListener / DatagramConn 接口，StreamDialer、StreamHandler
│   ├── socket.hpp             拥有 st_netfd 的 CocoSocket：超时和错误码；建 socket、bind、connect
│   ├── tcp.hpp  udp.hpp       TcpConn / TcpListener / TcpDialer，UdpConn / UdpListener
│   ├── tcp_server.hpp         TcpServer：accept 循环、每条连接一个协程、worker 线程、关停
│   ├── dns/
│   │   ├── codec/             message（RFC 1035 报文）、config（IpAddress、resolv.conf、hosts）、answer（从应答取地址）
│   │   └── resolver.hpp       Resolver：协程化的 UDP / TCP 查询、重试、缓存；LookupHost
│   ├── rudp/
│   │   ├── codec/             packet（16 字节报文头）、control（RudpOptions、RudpStats、RudpControl：一条连接的协议状态机）
│   │   ├── endpoint.hpp       内部：RudpEndpoint，一个 UDP socket、一条泵协程、按（地址, conn_id）找连接
│   │   └── conn.hpp           RudpConn、RudpListener、ListenRudp / DialRudp、RudpDialer
│   └── tls/                   config（TlsConfig）；conn（TlsConn、TlsDialer、TlsListener、TlsHandler）
└── app/                       在字节流上说话的应用层协议
    ├── http/
    │   ├── codec/             basic（状态码、方法、HttpHeader、HttpValues、转义）、message（HttpRequest / HttpResponse、解析、body 分帧）、
    │   │                      response（状态行、chunk 头、Date）、url（客户端 URL、重定向）
    │   ├── handler.hpp        HttpHandler、HttpError / HttpNotFound / HttpRedirect
    │   ├── response_writer.hpp HttpResponseWriter
    │   ├── mux.hpp            HttpServeMux
    │   ├── client.hpp         HttpClient、连接池
    │   └── server.hpp         ServeHttpConn、HttpServeOptions；HttpServer
    ├── ws/
    │   ├── codec/             frame（帧头、WebSocketFrameDecoder）、handshake（URL、Sec-WebSocket-Key / Accept）
    │   ├── conn.hpp           WebSocketConn
    │   ├── client.hpp         WebSocketClient
    │   └── handler.hpp        WebSocketHandler
    └── rtmp/
        ├── codec/             handshake、chunk、amf0、bytes、command（命令的解析和构造）、url（ParseRtmpUrl）
        ├── conn.hpp           RtmpConn
        ├── client.hpp         RtmpClient
        └── server.hpp         ServeRtmpConn、RtmpRequest、RtmpHandler；RtmpServer
```

一个协议的代码都在它自己的目录里，目录内部从下往上分三层，读一个协议也按这个顺序：

1. `codec/`：协议本身，不碰连接和协程，所以用一段内存里的字节就能测。大部分是字节和报文之间的转换；需要读写的（RTMP 握手和 chunk、HTTP body）只经过 `IoReader` / `IoWriter` / `BufReader` 接口。DNS 的 `config`（resolv.conf、hosts、地址字面量）也在这里：它只解析文本，不读文件。
2. 目录里的其余文件是会话：在一条 `StreamConn` 上驱动 codec，负责读写、状态和超时。
3. `server.hpp`：先是在一条连接上服务的函数（`ServeHttpConn`、`ServeRtmpConn`），再是用 `TcpServer` 把它组装成服务的 `HttpServer`、`RtmpServer`，加载证书、套 `TlsHandler()` 也在这里。和 Go 的 `net/http/server.go` 一样，`Server` 和逐连接的 `(*conn).serve` 放在同一个文件。

文件名只说明角色，不重复目录名或库名，所以 `app/http/server.hpp` 是 `HttpServer`，`app/rtmp/conn.hpp` 是 `RtmpConn`。

`HttpResponse` 是 codec 只用前置声明、不 include 连接的一个例外：它由 `codec/message` 里的解析器填写，声明也在那里，但它为客户端持有连接，和连接有关的成员（`Conn()`、`Reader()`、析构时回连接池）实现在 `app/http/client.cpp`。`HttpResponseWriter` 决定 Content-Length、chunked 还是关闭连接时要看请求和处理函数写了多少，所以留在会话里，只把状态行、chunk 头和 Date 的格式化放进 codec。

`StreamConn` / `StreamListener` / `DatagramConn` 在 `net/conn.hpp`，都是纯接口，不含 fd。`TcpConn` / `TcpListener` 在 `net/tcp.hpp`，`UdpConn` / `UdpListener` 在 `net/udp.hpp`，它们各自持有一个 `CocoSocket`，析构时关闭 fd。`TlsConn` 在 `net/tls/conn.hpp`，它拥有一条下层 `StreamConn`，自己不碰 fd。协程 ID 放在 `base/coroutine.hpp` 的 `CoroutineContext` 里。

`ListenTcp`、`DialTcp`、`ListenUdp`、`DialUdp` 返回错误码，成功时通过 `std::unique_ptr` 出参交出新连接。`DialTcp` / `DialUdp` 用本线程的 `DefaultResolver()` 解析主机名（见下文“协议”里的 DNS），`DialTcp` 依次尝试解析出的每个地址，IPv4 在前。监听地址必须是 IP 字面量，不经过解析。`StreamDialer` 是「给 host:port 建一条 `StreamConn`」的函数类型，`TcpDialer()` 用 `DialTcp` 实现它，`TlsDialer()` 在另一个 dialer 之上做 TLS 握手。`StreamHandler` 是「服务一条 `StreamConn`」的函数类型，`TcpServer` 对每条连接调用它；它可以一层套一层，像 Go 的 http middleware：`TlsHandler(cfg, next)` 先握手，再把 TLS 上的明文连接交给 `next`。

## 分层

```text
server     HttpServer、RtmpServer（server.*）       Serve 函数，以及组装：TcpServer + 可选 TlsHandler
app     |  app/ 下 HTTP、WebSocket、RTMP 的会话      只认 StreamConn 和 StreamDialer，不知道下面是 TCP 还是 TLS
tls     |  net/tls/：TlsConn、TlsDialer、TlsHandler  把一个 StreamConn 包成另一个 StreamConn
net        net/ 的其余部分：接口、TCP、UDP、RUDP、TcpServer、DNS 解析器    st_read / st_write / st_accept
codec      各协议的 codec/                            协议本身，不碰连接和协程
core       协程、日志、错误码、工具                   st_thread_create
```

`tls` 放在 `net/tls/` 目录里，但在分层上比 `net` 的其余部分高一层：`TcpServer`、socket、DNS 都不能 include 它。`app` 和 `tls` 平级，互不依赖，都只依赖 `net`，由各协议的 `server.*` 或调用方组合。这和 Go 一样：`net` 只有连接、监听和拨号，`crypto/tls` 在它上面把一条连接包成另一条，`net/http` 再决定什么时候用 TLS。HTTP 和 WebSocket 的客户端不自己建连，而是调用注入的 `StreamDialer`：默认 `TcpDialer()`，https / wss 传 `TlsDialer()`（对应 Go 的 `tls.Dialer`）。服务端：`TcpServer` 不认识 TLS，只对每条连接调用处理函数；`HttpServer::ListenAndServeTLS` 加载证书后把处理函数换成 `TlsHandler(cfg, ServeHttpConn…)`，对应 Go 的 `(*conn).serve` 一开始先 `tlsConn.Handshake()`。所以 OpenSSL 只出现在 `net/tls/` 里，`app` 和 `net` 的其余部分都没有；`tls` 也可以套在任何能产出 `StreamConn` 的东西上。Go 把 tls 放在 `crypto/` 下是标准库打包的历史原因，coco 没有别的密码学代码，按职责放进 `net`。

Go 服务端用 `tls.NewListener` 包住监听；coco 也有对应的 `TlsListener`，单线程时可以传给 `Serve`。但 `TcpServerOptions::threads` 大于 1 时，监听线程要把原始 fd 交给 worker，而 `TlsConn` 里的 ST 锁属于建它的线程、不能跨线程，所以握手必须在 worker 上做。`TlsHandler` 正是在连接自己的协程（也就是 worker）上运行，单线程和多线程都适用，`HttpServer` / `RtmpServer` 因此用它。

规则：

- 一个文件只能 include 同一层或层号更低的头文件；层号相同但层名不同的平级层（`app` 和 `tls`）互相不能 include。
- codec 还不能 include `base/`、`coco/coco_api.h` 和 `st.h`：协议本身不碰协程。它可以打日志、用 `utils/` 的 `IoReader` / `IoWriter`、`BufReader`。
- 库里的文件不能 include 汇总头文件 `coco/coco.h`，它包含所有层，会绕过上面的规则。
- 协议之间默认互不依赖，一个协议的 codec 也只给本协议用：`rtmp/codec/chunk` 不能用 `ws/codec/frame`，`net/`（包括 `net/tls/`）不能用 `app/http/codec/`。目前唯一的例外是 `ws` 可以用 `http`，因为 WebSocket 通过 HTTP Upgrade 建立。

这些规则由 ctest 里的 `LayerDependencies` 用例检查。它运行 `cmake/check_layers.cmake`，扫描 `src/` 下每一条 include，发现违规就列出文件并失败。层按路径判断：任何 `codec/` 目录下的文件都是 codec 层，任何目录下名为 `server.*` 的文件（`app/http/server.cpp`，或以后的 `net/dns/server.cpp`）是 server 层，所以在 `app/` 下加一个协议目录不用改脚本，只有协议之间要新的依赖时才在 `COCO_PROTO_ALLOWED` 里加一项。`net/tcp_server.*` 不叫 `server.*`，仍属于 net 层，不能用 TLS。`net/tls/` 不算协议目录，任何 `server.*` 都能用它。

各层对应的路径：

| 层 | 路径 |
| --- | --- |
| core | `base/`、`common/`、`log/`、`utils/` |
| codec | 任何 `codec/`：`net/dns/codec/`、`net/rudp/codec/`、`app/http/codec/`、`app/ws/codec/`、`app/rtmp/codec/` |
| server | 任何 `server.*`：`app/http/server.*`、`app/rtmp/server.*` |
| tls | `net/tls/` |
| net | `net/` 的其余部分 |
| app | `app/` 的其余部分，加上 http-parser |

`base/`、`log/`、`utils/` 互相引用（日志要取协程 ID，协程要打日志），所以合成一层 core。

构建上所有层编进同一个库 `libcoco`（CMake 目标 `coco::coco`），分层只靠上面的 include 规则保证，不再拆成每层一个库：拆开的好处是只用明文协议时可以不链接 OpenSSL，但代价是使用方要自己按顺序列出五个库再加上 ST 和 OpenSSL。ST、OpenSSL、http-parser 都是 `PRIVATE` 依赖：公共头文件里只有它们句柄类型的前置声明（`base/st_fwd.hpp`、`net/tls/config.hpp` 开头的 `SSL` / `SSL_CTX` / `BIO`），不 include 它们的头文件。

一条 TCP 连接的读路径是 `TcpConn::Read` → `CocoSocket::Read` → `st_read`。TLS 连接的明文读路径是 `TlsConn::Read` → `SSL_read`，缺密文时再调用下层的 `Read`（TCP 时就是上面那条路径）喂给 `bio_in`。

I/O 接口在 `src/coco/utils/io.hpp`：`IoReader`、`IoWriter`、`IoReaderWriter`。`BufReader`（`src/coco/utils/bufio.hpp`）是读缓冲，仿照 Go 的 `bufio.Reader`：协议解析在缓冲区里看字节、消费用掉的部分，剩下的留给同一条连接上的下一条报文。WebSocket 组包有 4MB 上限（`MAX_WS_PACKET`）。

## 层间接口

`app` 的服务端入口是一个函数，参数是一条已经建立好的 `StreamConn`，例如 `ServeHttpConn(StreamConn &conn, HttpHandler *handler)`。它不知道连接是怎么来的：可以是 TCP，可以是握手完成的 TLS，也可以是测试里的内存管道。TLS 同样只面对 `StreamConn`，输入一条，输出一条。

因此在任意两层之间插一层包装，就能观察或改变经过的字节，而不需要改协议代码：打印字节相当于在层间抓包，注入延迟或截断可以做故障实验。

## 并发模型

每个用到 coco 的线程各有一份 ST：程序从 `CocoRun()` 开始时由它初始化，也可以显式调用 `CocoInit()`，或者第一次建协程、建 socket、`CocoSleepMs` 时自动初始化。一个线程上，协程与线程是 1:N：多条协程，一个内核线程。默认栈 64KB。Linux 用 epoll，macOS 用 kqueue。要用满多核，`TcpServerOptions::threads`（`HttpServer` 是 `HttpServeOptions::threads`）让监听线程只 accept，连接交给连接数最少的 worker 线程（`CocoThread`）；对象不能跨线程，线程之间只交接裸 fd 和投递的函数，细节见 [协程与连接管理](coroutine.md) 的“多线程”一节。

服务端的结构由 `TcpServer` 固定下来：

1. 一条监听协程（`ListenRoutine`）循环调用 `StreamListener::Accept()`。`Accept` 持续失败时（例如 `EMFILE`）睡 10ms 再试，不会空转。`TcpServer::Start` / `Serve` 接受任何 `StreamListener`。
2. 每个新连接一条连接协程（`ConnRoutine`）。连接先设好 `TcpServerOptions` 的超时，再交给处理函数。处理函数是 `TlsHandler()` 时，TLS 握手就在这条协程上、受这些超时约束，握手失败就返回，不调用里面的处理函数；之后的读写只在这条协程里。
3. 处理函数返回后，连接在自己的协程里释放自己，并从 `ConnManager` 的名单里移除。连接对象（`StreamConn`）在 `DoCycle()` 末尾、协程还在跑 `Cycle()` 时就释放，所以它的析构函数里 `CocoShouldStop()` 仍反映 `Stop()`：TLS 和 RUDP 的析构据此决定要不要等对端。`TcpServer::Stop()` 和析构函数先停监听协程，再关闭监听 socket（新连接立刻被拒绝，端口马上可以重用），然后中断所有连接并等它们退出。两个协程同时调用 `Stop()` 时，后到的等先到的停完再返回，所以任何一个 `Stop()` 返回时服务都已经完全停下。
4. `ListenAndServe` / `Serve` 是 `Start` 加 `Wait()`：调用它的协程（通常是主协程）停在一个条件变量上，直到 `Stop()` 或退出请求。退出请求来自 `CocoShutdown()`（任何线程都能调）或 `SIGINT` / `SIGTERM`：信号处理函数只往一个 pipe 里写一个字节，由一条普通内核线程读出来再调用 `CocoShutdown()`，后者把请求送到每个有运行时的线程，由各线程自己的协程完成关停，所以真正的关停逻辑都跑在普通协程上，不在信号上下文里。信号在第一次等待（或 `CocoRun`）时才接管：第一个信号请求退出，并把信号还给原来的处理方式；第二个信号直接按默认动作结束进程。读信号的不是协程，所以即使某段代码一直不让出也有效；启动时就被忽略的信号保持忽略。
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
- **DNS**：在 `src/coco/net/dns/`。原来解析用 `getaddrinfo`，它是阻塞的系统调用，解析期间整条 ST 线程停住；现在 `Resolver` 自己发查询，等应答时只让出当前协程。
  - 顺序：IP 字面量（也接受带方括号的 IPv6 和 `%zone`）原样返回；然后查 hosts 文件；`localhost` 和 `*.localhost` 不在 hosts 里时固定是 127.0.0.1 / ::1（RFC 6761），不问服务器，免得 search 后缀把它变成别人的域名；最后按 resolv.conf 的规则生成候选名：结尾带点的只查它自己，点数不少于 `ndots` 的先查原名再加 search 后缀，否则先加后缀、原名放最后。
  - 查询：每个候选名把 A 和 AAAA 从同一个 UDP socket 一起发出（`AF_INET` / `AF_INET6` 只发一个），ID 随机，源端口由内核随机分配。只接受来自所问服务器地址和端口（IPv6 还比较 scope ID）、ID 和问题都对得上、opcode 为 QUERY 且只有一个问题的应答，其余的丢掉继续等。标签内不能含点或 NUL，名字解压最多迭代 255 次。应答带 TC 位时，在 `TaskGroup` 子协程里对同一服务器改用 TCP（2 字节长度前缀）重问，另一类记录的 UDP 应答仍可处理；TCP 应答仍带 TC 时不算成功，也不缓存。SERVFAIL、REFUSED、发送失败转下一个服务器；NXDOMAIN 和“有这个名字但没有这种记录”都算有结论。每个服务器共用一个由 `timeout_us` 确定的绝对截止时间，TCP 的每次读取都受它约束，整个列表最多过 `attempts` 遍。
  - 结果：只取问题名以及从它出发的 CNAME 链上的名字拥有的记录，别的名字的记录忽略，CNAME 成环则报错；`AF_UNSPEC` 时 IPv4 在前。成功的结果按 TTL 缓存（最多一小时，最多 4096 条），有效期从收到应答时起算，等待另一类记录的时间也计入 TTL；失败不缓存。
  - 配置：`Resolver()` 跟随 `/etc/resolv.conf`（`nameserver`、`domain`、`search`、`options ndots / timeout / attempts`）和 `/etc/hosts`，最多每 5 秒按 mtime（含纳秒）、大小、inode 检查一次是否变了，resolv.conf 变了就清缓存，使用旧配置快照的在途查询也不能把结果写回缓存。没有 `nameserver` 时用 127.0.0.1 和 ::1。读这两个文件是普通文件 I/O，没走协程。`Resolver(DnsConfig)` 用给定的配置，不读文件，测试就是这样把它指向本机的假服务器。
  - 线程：`Resolver` 属于创建它的线程，同一线程上的多条协程可以同时用它；正在等服务器的查询持有开始时的配置和 hosts 的 `shared_ptr`，期间别的协程重新加载文件也不影响它。`DefaultResolver()` 每个线程一个，不销毁。协程被中断时先取消并等待所有 TCP 子任务结束，再返回 `ERROR_THREAD_INTERRUPED`；即使中断恰逢最后一个任务结束，任务组等待也会记录取消状态。
  - 不做的：EDNS0、DNSSEC、DoT / DoH、`nsswitch.conf` 里其他来源（mDNS 的 `.local`、LDAP）、macOS 的分域解析（`scutil --dns` 里按域名指定服务器的 resolver，VPN 常用）、IDN，以及同一线程上对同一名字的并发查询合并。
- **RUDP**：在 `src/coco/net/rudp/`，设计、线上格式和全部规则见 [RUDP](rudp.md)。UDP 上有序、可靠的字节流，对外是 `StreamConn`：`RudpListener` 是 `StreamListener`（交给 `TcpServer`，只能 `threads <= 1`），`RudpDialer()` 是 `StreamDialer`，所以 HTTP、TLS 等不改代码就能跑在上面。
  - 协议：16 字节头；三次握手，服务端等客户端证明收到 SYN_ACK 后才把连接交给 `Accept`；按段编号，ISN 随机；每个数据段单独确认并带累计确认；RFC 6298 的 RTO（Karn，每段各自退避，单次重传间隔不超过链路超时的三分之一），收到 3 个后续段的确认就快速重传（每段最多一次）；接收窗口按段计，零窗口时放行一个段做探测（探测超时不算拥塞，窗口重开时立即重发）；拥塞控制是最简 AIMD，一轮丢包只减一次窗口，超时降到 1。有未确认的东西且对端沉默超过 `link_timeout_us` 就算断链；关闭另外从调用起最多等 `link_timeout_us`，对端一直回 ACK 却不读也不例外。读写和拨号的超时是整次调用的预算，从调用时算起，不会因为 ST 从旧时钟起算而提前到期。
  - 协议状态机 `RudpControl` 在 codec 里，输入报文和当前时间、输出要发的数据报，不碰 socket、协程和时钟，测试用假时钟和内存链路驱动。
  - 会话：每个 UDP socket 一个 `RudpEndpoint`，由句柄（`RudpListener`、各 `RudpConn`）通过 `shared_ptr` 共同拥有；它的泵协程读 socket、把报文交给对应连接、每 `interval_us` 跑一次定时器，每处理 64 个数据报让出一次。泵不持有端点的引用，在任何让出之后都不再用让出前拿到的连接条目；端点析构时先取消并等待泵，再关 socket。监听销毁后拒绝新连接、重置未接受的连接，已接受的照常工作，端口在最后一条连接析构后释放。
  - 关闭：`Close()` 在本端写的每个字节都被确认、并且本端 FIN 被确认或收到了对端的 FIN 或 RST 后成功，不需要 TIME_WAIT；析构时调用它，最长等链路超时。协程已被要求停止（`CocoShouldStop()`）或等待中被中断时不等，直接 RST。读到对端 FIN 之后 `Read` 返回 `ERROR_SOCKET_READ` 且 `*nread == 0`，和 TCP 一样，HTTP 读到关闭为止的 body 和连接池重试都依赖这一点。
  - 不做的：pacing 和更好的拥塞算法、路径 MTU 探测（固定 MSS 1200）、保活、半关闭、连接迁移、加密认证、防反射放大、跨线程。
- **TLS**：服务端和客户端都有，可以套在任何 `StreamConn` 上。`TlsConfig` 共享 `SSL_CTX`，证书只加载一次。握手不绑定 TLS 1.2 的报文轮次，1.2 和 1.3 都能完成。证书校验是 `SSL_VERIFY_NONE`。
- **HTTP/1.1**：接口仿照 Go 的 `net/http`。
  - 服务端：处理函数是 `HttpHandler::ServeHTTP(HttpResponseWriter &w, HttpRequest &r)`，或者用 `HandleFunc` 注册 lambda。`HttpServeMux` 支持 Go 1.22 的模式语法：可带方法（`"GET /users/{id}"`，GET 也接 HEAD）、主机、`{name}` 单段通配、`{name...}` 尾段、以 `/` 结尾的子树和 `{$}`。路由是按路径段建的树，越具体越优先：字面段优先于 `{name}`，再优先于子树；带方法的优先于不带方法的。和 Go 一样，不带方法的模式接受任何方法；只有路径匹配而所有模式的方法都不匹配时才回 405 和 `Allow`。含 `.`、`..`、`//` 的路径先 301 到规范形式；注册了 `/tree/` 而没有模式精确匹配 `/tree` 时，`/tree` 301 到 `/tree/`。`HttpServer` 是 `TcpServer` 加 `ServeHttpConn`，`ListenAndServe` / `ListenAndServeTLS` 一直服务到 `Stop()` 或退出请求，`Start` / `StartTLS` 开始服务后立即返回；HTTPS 由 `StartTLS` 加载证书、把处理函数换成 `TlsHandler(cfg, ServeHttpConn…)` 提供，证书加载失败时 `StartTLS` 直接返回 `ERROR_HTTPS_KEY_CRT` 并关闭刚监听的端口；握手在 `ServeHttpConn` 开始之前完成。
  - 连接循环：一条连接在整个生命周期里只用一个 `BufReader`、一个 `HttpResponseWriter` 和一个 `HttpRequest`，缓冲区跨请求复用，流水线请求中提前读到的字节不会丢。请求头只把头部字节交给 http-parser 解析，body 由 `HttpBodyReader` 按 Content-Length 或 chunked 从同一个缓冲区读。处理函数没读完的 body 在 256KB 以内会被跳过以保住 keep-alive，更长就关连接。`Expect: 100-continue` 在处理函数第一次读 body 时才回 100。请求头超限回 431，格式错误或 HTTP/1.1 缺 Host 回 400，之后关连接。
  - 写响应：语义同 Go 的 `ResponseWriter`。写入先进 4KB 缓冲；处理函数返回时还没超过缓冲区、又没设 Content-Length，就自动补上 Content-Length，状态行、头部和 body 一次写出；超过缓冲区或调用 `Flush()` 后改成 chunked（HTTP/1.0 则以关连接结束），大块数据用 `writev` 直接发出，不拷进缓冲区。没设 Content-Type 时按前 512 字节嗅探，自动加 Date。`Hijack()` 交出连接和读缓冲，之后服务端不再碰这条连接，WebSocket 就是这样接管的；Upgrade 请求没被接管时，响应后关连接。
  - 客户端：`HttpClient` 仿照 Go 的 `http.Client`，`Get` / `Post` / `Do(HttpRequest&)` 返回 `std::unique_ptr<HttpResponse>`，从 `resp->body` 读 body。keep-alive 连接按 `scheme://host:port` 放进连接池复用（每个主机默认留 2 条空闲）；`HttpResponse` 析构时，body 已读完（或者剩下的部分已经全在缓冲区里）就把连接还回池子，否则关掉。从池里取出的连接若已被服务端关闭，请求会在新连接上重试一次：写失败时总是重试，读失败时只重试 GET、HEAD、OPTIONS、TRACE。默认跟随最多 10 次重定向，301/302/303 把非 GET/HEAD 改成不带 body 的 GET，跨主机时去掉 Authorization 和 Cookie。连接由注入的 `StreamDialer` 建立，`http://` 默认 `TcpDialer()`，`https://` 需要 `SetTlsDialer(TlsDialer())`。`HttpGet` / `HttpPost` 用一个共享的默认客户端。
- **WebSocket**：在 `src/coco/app/ws/`。`WebSocketConn` 是握手之后的一条连接，客户端和服务端共用。一条协程用 `ReadMessage()` 同步读下一条数据消息（分片已拼好），PING 和 CLOSE 在读的过程中顺带回复，所以总得有协程在读；对端的 CLOSE 要等它前面的消息都被读走后才回，这样对这些消息的回复能先发出去。`Send` 可以在任意协程调用，整帧一次写出并用锁串行化，所以不会和 PONG 交错。客户端 `WebSocketClient` 用 HTTP 升级握手，发送的每一帧用随机掩码，有两种用法：仿照 Go 的 `Dial("ws://host:port/path")`（`wss://` 要先 `SetTlsDialer(TlsDialer())`，否则返回 `ERROR_HTTPS_NOT_SUPPORTED`）之后由调用方自己循环 `ReadMessage()`，析构时连接还开着就发 CLOSE 1000；或者 `Start()`，另起一条读协程循环读，把消息交给 `SetMessageHandler` 的回调，这时不能再调 `ReadMessage()`。服务端仿照 Go：`WebSocketHandler` 包一个 `void(WebSocketConn *)` 函数，注册到 `HttpServeMux` 上，`HttpServer`，或处理函数是 `TlsHandler(cfg, ServeHttpConn…)` 的 `TcpServer`（wss）都能用。它校验升级请求（`GET`、`Upgrade: websocket`、`Connection: upgrade`、16 字节的 `Sec-WebSocket-Key`、版本 13），不合格回 400；握手成功后在这条 HTTP 连接的协程里调用这个函数，函数就是连接的生命周期，返回时若连接还开着就发 CLOSE 1000，再等还卡在 `Send` 里的协程（最多等到发送超时），然后释放连接。别的协程可以拿着 `WebSocketConn*` 推送，但只能到这个函数返回为止。服务端发送不加掩码，收到不带掩码的帧按 1002 关闭。握手时 `WebSocketHandler` 用 `Hijack()` 接管连接，之后 `ServeHttpConn` 不再按 HTTP 读它。帧的编解码在 `ws/codec/frame.cpp`，握手的 URL 解析和 Key / Accept 计算在 `ws/codec/handshake.cpp`：`WebSocketFrameDecoder` 自己缓存不完整的帧，把分片拼成完整消息，分片之间插入的控制帧单独交出；违反 RFC 6455 的帧（保留位或 opcode、分片或超过 125 字节的控制帧、单帧或消息超过 `MAX_WS_PACKET`）会让连接回 1002 / 1009 后关闭。
- **RTMP**：在 `src/coco/app/rtmp/`。握手先认复杂握手（HMAC-SHA256 digest，两种 scheme 都接受），对不上再退回简单握手（版本字段为 0，S2/C2 回显对端的 1536 字节）。`RtmpConn` 在一条 `StreamConn` 上收发已经按 chunk 拼好的消息；chunk size、acknowledgement、peer bandwidth 和 ping 在 `ReadMessage` 里处理，写出整条消息一次完成并用锁串行化，所以应答不会和业务写交错。`ServeRtmpConn` 完成 connect 和 createStream，收到 publish 或 play 后先回 NetStream 状态，再把连接交给处理函数。`RtmpClient` 解析 `rtmp://host[:port]/app/stream`（第一段是 app，其余是流名）；`rtmps://` 要先 `SetDialer(TlsDialer())`。`RtmpServer` 是 `TcpServer` 加 `ServeRtmpConn`，`ListenAndServeTLS` 就是 RTMPS。单条消息受 24 位长度限制（`kRtmpMaxMessage`，16777215 字节）。不实现 RTMPE 和 shared object，aggregate 消息原样交给调用方。

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
| 4071–4075 | DNS | `ERROR_DNS_NOT_FOUND` 4071，`ERROR_DNS_TIMEOUT` 4075 |
| 4081–4083 | RUDP | `ERROR_RUDP_RESET` 4081（对端 RST），`ERROR_RUDP_TIMEOUT` 4082（握手无应答或链路超时），`ERROR_RUDP_CLOSED` 4083（本端已关闭） |

文件里还有一批系统错误码（pid 文件、带宽限制等），当前网络路径不会返回它们。

## 示例与测试

`tests/` 下是 ctest 用例，`./build.sh -t` 会跑它们。`coroutine_test.cpp` 覆盖协程和 `ConnManager` 的生命周期；`tcp_server_test.cpp` 覆盖 `TcpServer` 的回显、关停、处理函数返回、TLS、`CocoShouldStop()`，以及 `TlsHandler`：握手失败（对端不说 TLS）时不调用里面的处理函数、关闭连接并继续接受，超时在处理函数之前设好、能截住不发 ClientHello 的对端，`Stop()` 能中断卡在握手里的连接并等它退出；`tls_test.cpp` 覆盖 `TlsConn` 的读写和并发写、`TlsListener`、`HttpServer::StartTLS`，以及证书加载失败时 `HttpServer` / `RtmpServer` 的 `StartTLS` 直接失败并关闭端口；`ws_test.cpp` 覆盖帧的编解码（任意切分、分片与控制帧交错、非法帧）、客户端对 PING / CLOSE 的回复、`Dial` 的 URL 解析和析构时的 CLOSE 1000，以及读协程退出时仍有协程阻塞在 `Send` 里的情况；`ws_server_test.cpp` 覆盖服务端的握手（大小写不同的头、紧跟在请求后面的帧）、非法升级回 400、PING / CLOSE（和 CLOSE 同包到达的消息仍会被读到）、拒收不带掩码的帧、处理函数返回时发 CLOSE 1000、关停时结束已打开的连接，以及 wss；`http_test.cpp` 覆盖 HTTP 的响应分帧（自动 Content-Length、chunked、Flush）、流水线、各种请求 body 与未读 body 的跳过、100-continue、HEAD、HTTP/1.0、431/400/417、路由规则和 405、请求字段的解码，以及客户端的连接复用、过期连接重试、重定向和响应分帧；`lifecycle_test.cpp` 通过 `HttpServer`、`WebSocketClient` 走一遍关停和对端关闭的路径；`thread_test.cpp` 覆盖两个线程各跑一套运行时、`CocoThread` 的投递 / 中断 / 停止（`Stop()` 只挂起调用方、并发 `Stop()`、退出请求中断投递的函数）、从普通线程调用 `CocoShutdown()`、fd 在线程之间交接，以及多线程的 `TcpServer`（按负载分配、TLS、退出请求、只接受 `TcpListener`）和 `HttpServer`；`task_group_test.cpp` 覆盖 `TaskGroup` 的等待、首个错误、取消（含取消后才起的函数）、析构时取消并等待、等待方被中断时取消整组（包括 `CocoRun` 主体收到退出请求），`CocoYield()` 让停止请求进入不做 I/O 的循环，以及 `CocoThread` 的 `Call()`（从协程和普通线程）、负载上限、未启动就 `Stop()` 时丢弃排队的函数、退出请求之后投递的函数一启动就是中断状态；`runtime_test.cpp` 覆盖 `CocoInit()` 的幂等、别的线程有自己的运行时、调试构建里跨线程使用对象会断言失败、`SIGTERM` 让多线程服务以状态 0 退出、`CocoShutdown()` 和信号唤醒等待者、阻塞式 `ListenAndServe` 在 `Stop()` / 退出请求 / 处理函数里 `CocoShutdown()` 时返回、`Stop()` 关闭监听端口且并发调用安全，以及 `CocoRun` 的各条退出路径；其中两个用例起子进程发真实信号：第一个 `SIGTERM` 让服务以状态 0 退出，第二个 `SIGINT` 结束一个一直不让出协程的进程；`LayerDependencies` 检查分层。`rtmp_test.cpp` 覆盖 AMF0、chunk（含扩展时间戳和交错）、URL，以及本机推流再拉流。`dns_test.cpp` 覆盖 DNS 报文的编解码（压缩指针、指针成环和向前指、超长名字、任意截断、opcode / 问题数 / 标签校验、解压迭代上限）、resolv.conf 和 hosts 的解析、字面量 / hosts / localhost，以及对着同一线程上一个假 DNS 服务器的查询：A 和 AAAA 一起发、缓存及 TTL 从应答到达起算、配置重载后旧查询不回填缓存、CNAME 链之外的记录被忽略和成环报错、search 列表的顺序、SERVFAIL 转下一个服务器、丢包重试和超时、伪造的应答（含 IPv6 scope 不符）被忽略、截断后改用 TCP（慢 TCP 不挡 UDP 应答、逐段读取共用截止时间、仍带 TC 被拒绝）、查询期间别的协程照常运行、被中断时取消并等待 TCP 子任务结束，以及 `DialTcp("localhost")`。这些用例不访问外网。`rudp_test.cpp` 先在假时钟和内存链路上测协议状态机：报文编解码与非法报文、握手各包丢失和重复 SYN、无丢包传输、丢包乱序重复、突发丢包（每段快速重传一次、一轮只减一次窗口）、拥塞窗口的慢启动 / 拥塞避免 / 减半 / 超时降到 1、Karn 与每段退避、零窗口探测（应用长时间不读不算断链、窗口更新丢了也能恢复）、链路超时的边界、序号回绕（含非 2 的幂的接收窗口）、关闭完成的各条规则、对端不读时关闭仍在链路超时内结束、零窗口不算拥塞、FIN 之后的段不交付、窗口外和越界确认；再在本机 UDP 上测连接：回显、经过丢包乱序的中继、拨号超时 / 被拒绝 / 被中断（RST 立即释放半开名额）、读超时不被多余的唤醒重置、中断与数据同一次交接时中断优先且数据保留、另一条协程关闭时读立即结束、调用前长时间不让出时超时也不早到、写在窗口满时阻塞与超时、三种关闭顺序、丢包下关闭仍送达全部数据、对端消失时关闭在链路超时后失败、`TcpServer::Stop()` 让连接 RST 而不等关闭握手、监听销毁后已接受的连接照常工作、半开名额、泵在洪泛下让出、`threads > 1` 被拒绝，以及 HTTP（含读到关闭为止的 body 和连接池重试）和 TLS over RUDP。`examples/` 里的程序（TCP/UDP echo、HTTPS 服务端和客户端、WebSocket 客户端和回显服务端、RTMP 直播转发、`dns/lookup` 并发解析主机名，以及 `threads/` 下对照单线程、多线程服务器、`CocoThread` 和普通线程的四个程序）用来手动验证。
