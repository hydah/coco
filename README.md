# coco

[English](README_en.md) | 中文

coco 是一个基于 [State Threads](https://github.com/hydah/state-threads)（ST）的 C++11 网络库。每条连接跑在自己的协程里，代码按同步方式写：`Read` 没有数据时，当前协程让出，事件循环去跑别的协程，数据到了再切回来。不需要回调，也不需要手写状态机。

支持 TCP、UDP、RUDP（UDP 上的可靠字节流）、TLS、HTTP/1.1、WebSocket 和 RTMP，主机名由自带的 DNS 解析器在协程里解析，运行在 Linux（epoll）和 macOS（kqueue）上。

## 特性

- **同步写法，异步执行**：一个进程一个内核线程，上面跑多条协程，默认栈 64KB。
- **协议齐全**：TCP / UDP、TLS 1.2 / 1.3（服务端和客户端）、HTTP/1.1（keep-alive、chunked、路由）、WebSocket（`ws://` 和 `wss://`，服务端和客户端）、RTMP（`rtmp://` 和 `rtmps://`，推流和拉流）。
- **RUDP**：UDP 上有序、可靠、带流量控制和最简拥塞控制（AIMD）的字节流。`RudpListener` 交给 `TcpServer`、`RudpDialer()` 交给 `HttpClient` / `TlsDialer`，HTTP、WebSocket、RTMP、TLS 不改代码就能跑在上面。
- **解析域名不卡线程**：`DialTcp` 和各个客户端用自带的 DNS 解析器（读 `/etc/resolv.conf` 和 `/etc/hosts`，A / AAAA 同时查，按 TTL 缓存），等待应答时只让出当前协程，不像 `getaddrinfo` 那样让整个线程停住。
- **按协议分层**：每层一个目录，只能依赖下层。HTTP 和 WebSocket 只认 `StreamConn`，不关心下面是 TCP 还是 TLS。分层规则由测试强制检查。
- **连接生命周期由框架管理**：`TcpServer` 负责 accept、TLS 握手、连接回收和关停，业务只写一个处理函数。
- **错误码而非异常**：所有接口返回 `int`，`COCO_SUCCESS` 为 0。
- **可以直接当库用**：一个 `libcoco`，名字都在 `namespace coco` 里，公共头文件不暴露 ST、OpenSSL、http-parser；支持 `find_package(coco)`、`add_subdirectory` 和 pkg-config。
- **依赖自带**：ST 和 http-parser 是 submodule，OpenSSL 在构建时从源码编成静态库。

## 快速开始

```bash
git clone --recursive https://github.com/hydah/coco.git
cd coco
./build.sh            # Release 构建，产物在 build/bin/
./build.sh -t         # 构建并运行测试
```

已经 clone 过但没拉 submodule 时，先执行 `git submodule update --init --recursive`。第一次构建会从 GitHub 下载 OpenSSL 源码包，离线构建方法见 [构建文档](.harness/docs/build.md)。

跑一个 TCP 回显：

```bash
./build/bin/pingpong_server_tcp    # 终端 1，监听 127.0.0.1:8080
./build/bin/pingpong_client_tcp    # 终端 2
```

## 代码示例

### TCP 回显服务

```cpp
#include "coco/coco.h"

int main() {
    return coco::CocoRun([]() {
        coco::TcpServer server([](coco::StreamConn &conn) {
            char buf[1024];
            ssize_t n = 0;
            int ret;
            while ((ret = conn.Read(buf, sizeof(buf), &n)) == COCO_SUCCESS) {
                if ((ret = conn.Write(buf, n, nullptr)) != COCO_SUCCESS) break;
            }
            return ret;
        });
        // 一直服务到 Ctrl-C / SIGTERM，关闭所有连接后返回
        return server.ListenAndServe("127.0.0.1", 8080) == COCO_SUCCESS ? 0 : 1;
    });
}
```

`CocoRun` 是程序的入口：它在当前线程上建好协程运行时，再调用传进去的函数。每条新连接都会在一条独立的协程里调用处理函数，函数返回后连接自动释放。连接的状态直接放在局部变量里即可。

`coco/coco.h` 包含全部公共接口，所有名字都在 `namespace coco` 里（下面的片段省略了 `coco::`）；宏以 `COCO_` 或 `coco_` 开头。

### 运行时与退出

程序用 `return CocoRun([]() { ... });` 开始，所有 coco 调用都写在里面。`CocoRun` 在调用它的线程上初始化 ST，失败时直接返回错误码，并让程序主体能被 Ctrl-C 打断（见下一节）。不用 `CocoRun` 也能工作：第一次用到协程或 socket 的调用会自动初始化，想单独检查初始化是否成功时可以先调 `CocoInit()`。

每个用到 coco 的线程各有一份运行时，第一次用到时自动初始化。coco 创建的对象只能在创建它的那条线程上使用；怎么让多个线程一起干活，见后面的“多线程”一节。

`ListenAndServe` / `ListenAndServeTLS` / `Serve` 会阻塞，直到 `Stop()` 或收到退出请求（`SIGINT`、`SIGTERM` 或 `CocoShutdown()`），返回前先关闭监听端口、等所有连接退出，所以 `main` 可以直接 `return`。同时跑多个服务，或者服务之外还有别的事要做时：

```cpp
HttpServer api(&mux);
TcpServer echo(Echo);
api.Start("0.0.0.0", 8080);    // Start / StartTLS 开始服务后立即返回
echo.Start("0.0.0.0", 9000);
CocoWaitForShutdown();         // 等到 Ctrl-C；析构时各自关停
```

处理函数里不能调用 `Stop()`（它要等所有连接退出，包括自己），要结束程序就调 `CocoShutdown()`。

信号的规则：第一个 `SIGINT` / `SIGTERM` 请求优雅退出，同时信号恢复成原来的处理方式；第二个信号一定会结束进程，即使关停卡住了，或者某段代码一直不让出协程、第一个信号根本没被处理。启动时就被忽略的信号（shell 里后台作业的 `SIGINT`）保持忽略。

### 让主循环也能被 Ctrl-C 打断

服务端靠阻塞的 `ListenAndServe` 就能优雅退出。自己写主循环（例如一个不断读写的客户端）时，`CocoRun` 还负责让循环停下来：它调用 `fn` 并返回它的返回值；`fn` 运行期间收到退出请求，`CocoShouldStop()` 变为 true，正在阻塞的那个调用失败一次，循环自己结束，`fn` 栈上的对象照常析构。

```cpp
int main() {
    return CocoRun([]() {
        std::unique_ptr<TcpConn> conn;
        if (DialTcp("127.0.0.1", 8080, 1000 * 1000, &conn) != COCO_SUCCESS) return 1;
        while (!CocoShouldStop()) {      // Ctrl-C、SIGTERM 或 CocoShutdown() 之后为 true
            // conn->Write()、conn->Read()、CocoSleepMs() ...
        }
        return 0;                        // conn 在这里正常关闭
    });
}
```

`fn` 就跑在主协程上，用的是进程自己的栈，不是 64KB 的协程栈。只有正在进行的那一次阻塞调用会失败，之后的调用照常工作，所以循环要自己检查 `CocoShouldStop()`。`examples/pingpong` 里的两个客户端就是这样写的。

### 一个线程上同时做几件事

`TaskGroup` 在当前线程上为每个函数起一条协程，并统一取消、统一等待。它不需要继承，也不需要手动 `delete`：析构时先取消再等所有函数返回，所以这些函数可以放心使用所在作用域栈上的变量。

```cpp
return CocoRun([&]() {
    TaskGroup tasks;
    tasks.Spawn([&]() { return ReadLoop(conn); });
    tasks.Spawn([&]() { return Heartbeat(conn); });
    return tasks.Wait();         // Ctrl-C 时 CocoRun 的主体被中断，Wait 会取消整组再等它们退出
});
```

- `Cancel()` 中断组里的每个函数，但不等待；之后再 `Spawn` 的函数一启动就是中断状态。
- `Wait()` 只挂起调用它的协程，返回第一个出错函数的返回值。调用方自己被中断（或 `CocoShouldStop()` 为 true）时，`Wait()` 会先取消整组再接着等，所以停止请求会逐层传到子任务。
- 某个函数如果不响应中断，`Wait()` 和析构就会一直等下去。

调度是协作式的：一条协程不阻塞、也不让出，同一线程上的其他协程、I/O 和停止请求就全都进不来，连检查 `CocoShouldStop()` 也看不到跨线程送来的请求。不做 I/O 的长循环每隔几毫秒调一次 `CocoYield()`，再检查 `CocoShouldStop()`；真正耗 CPU 的计算交给单独的线程（见下面的 `CocoThread::Call()`），别放在服务 I/O 的线程上。

### 多线程

一个线程上的所有协程只用一个核。要用满多核，最简单的是让服务器把连接分给几个 worker 线程：

```cpp
TcpServerOptions opt;
opt.threads = 4;                 // 本线程只 accept，连接交给连接数最少的 worker
TcpServer server(Echo, opt);     // HttpServer 用 HttpServeOptions::threads
return server.ListenAndServe("0.0.0.0", 8080);
```

这时处理函数会在多个线程上同时被调用，它用到的共享数据要能跨线程使用（自己加锁或用原子变量）。注意 pthread 锁一旦等待，挡住的是那个线程上所有的协程，所以临界区要短。TLS 把处理函数套一层：`TcpServer server(TlsHandler(cfg, Echo), opt)`（`HttpServer` 直接 `ListenAndServeTLS`），握手在 worker 上、连接自己的协程里做。`Stop()` 和退出请求会让各个 worker 中断自己的连接、等它们退出，再结束线程。`RtmpServer` 目前还是单线程：推流和拉流必须在同一个线程上才能转发。

也可以直接用 `CocoThread`，它是一个跑着自己运行时的内核线程：

```cpp
CocoThread worker;               // CocoThread worker(1000)：最多 1000 个未返回的函数
worker.Start();
worker.Post([]() { /* 在 worker 上的一个新协程里运行，可以阻塞在 I/O 上 */ });
long n = 0;
int ret = worker.Call([&]() { n = Compute(); return COCO_SUCCESS; });  // 等它返回，拿到返回值
worker.Stop();                   // 中断还在跑的函数，等它们返回，再 join 线程
```

`Post()` 和 `Call()` 可以在任何线程上调用，其他接口只能在创建它的线程上用。

- **`Post()`** 从不阻塞，返回成功只代表函数已经入队。投递的函数各自跑在一条协程上，是并发执行的，不是排队一个接一个地跑。
- **`Call()`** 只挂起调用它的协程（没有运行时的线程则阻塞整个线程），直到函数返回，再把返回值交回来。函数可以使用调用方栈上的变量，所以调用方被中断也不会提前返回。每次调用要用一个 pipe，适合偶尔发一次请求，不适合每条消息都用。
- **负载上限**：构造时给了上限的话，未返回的函数达到上限时，`Post()` / `Call()` 返回 `ERROR_THREAD_BUSY`。

投递的函数和服务器的处理函数一样，被中断时要能结束（阻塞调用失败一次，`CocoShouldStop()` 变为 true）。`Stop()` 时会中断；收到退出请求时也会中断，包括请求之后才投递的函数，它们一启动就是中断状态，和退出请求之后才开始运行的 `CocoRun` 主体一样。线程本身则一直运行到 `Stop()`。

`Stop()` 只挂起调用它的那个协程，同一线程上的其他协程照常运行。它没有超时：有函数不理会中断时会一直等下去。一直没启动的线程，`Stop()` 时把排队的函数直接销毁，不会运行它们。

线程之间只交接两样东西：

- **投递的函数**：`CocoThread::Post()` / `Call()`。函数捕获的东西随它一起过去，但 coco 创建的对象（socket、连接等）仍然只属于创建它的线程。
- **裸 fd**：`TcpConn::Release()` 交出 fd 但不关闭它，另一个线程用 `TcpConnFromFd()` 在自己的运行时上包回来。TLS 会话不能换线程，要在交接之后再包 `TlsConn`。

怎么选：只在一个线程里干活就用 `CocoRun`；服务器想用满多核，加 `threads` 选项；要把任务或连接交给别的线程，用 `CocoThread`；几个线程各干各的、互不派活，普通 `std::thread` 里各自 `CocoRun` 也行。`examples/threads/` 下的 `threads_single`、`threads_server`、`threads_post`、`threads_plain` 分别演示这四种，运行后输出里标出每一步在哪个线程。

`CocoShutdown()` 和 `CocoShutdownRequested()` 是整个进程共用的，可以在任何线程（包括 coco 不知道的线程）上调用，所有线程上的等待者和 `CocoRun` 都会收到。除此以外，连接、监听、服务器、`HttpClient`、`WebSocketClient` 这些对象都不能跨线程使用；不带 `NDEBUG` 的构建里，跨线程使用协程、socket 或 `CocoThread` 会直接断言失败。`HttpGet` / `HttpPost` 用的默认客户端每个线程各一个。

### HTTP / HTTPS 服务

```cpp
HttpServeMux mux;
// 模式语法同 Go 1.22 的 ServeMux：可带方法、{name} 通配段、{path...} 尾段，/static/ 匹配子树
mux.HandleFunc("GET /hello/{name}", [](HttpResponseWriter &w, HttpRequest &r) {
    w.Write("hello " + r.PathValue("name"));   // 小响应自动带 Content-Length，一次写出
});
mux.HandleFunc("POST /echo", [](HttpResponseWriter &w, HttpRequest &r) {
    std::string body;
    r.body.ReadAll(&body);
    w.Header().Set(HttpHeaderContentType, HttpContentTypeJson);
    w.Write(body);
});

HttpServer server(&mux);
server.ListenAndServe("0.0.0.0", 8080);
// HTTPS：server.ListenAndServeTLS("0.0.0.0", 9082, "server.crt", "server.key");
```

客户端仿照 Go 的 `http.Client`：按主机复用 keep-alive 连接，自动跟随重定向。HTTPS 需要注入 `TlsDialer()`：

```cpp
HttpClient client;
client.SetTlsDialer(TlsDialer());   // 只用 http:// 时可以不设

std::unique_ptr<HttpResponse> resp;
if (client.Get("https://127.0.0.1:9082/hello/coco", &resp) == COCO_SUCCESS) {
    std::string body;
    resp->body.ReadAll(&body);   // resp 析构时连接回到连接池
}

HttpRequest req("PUT", "http://127.0.0.1:8080/items/1", "{\"n\":1}");
req.header.Set("Content-Type", "application/json");
client.Do(req, &resp);
// 共享的默认客户端：HttpGet(url, &resp)、HttpPost(url, type, body, &resp)
```

### WebSocket 服务端与客户端

```cpp
// 服务端：处理函数就是这条连接的生命周期，返回时自动发送 CLOSE 1000
HttpServeMux mux;
mux.Handle("/echo", new WebSocketHandler([](WebSocketConn *ws) {
    std::string data;
    WebSocketHeader::Type type;
    while (ws->ReadMessage(&data, &type) == COCO_SUCCESS) {
        ws->Send(data, type);
    }
}));
HttpServer server(&mux);   // wss 用 ListenAndServeTLS
server.ListenAndServe("0.0.0.0", 9083);
```

```cpp
// 客户端：用法仿照 Go；wss:// 需要先注入 TLS dialer
WebSocketClient ws;
ws.SetTlsDialer(TlsDialer());
ws.Dial("ws://127.0.0.1:9083/echo");
ws.Send("hello");
std::string reply;
ws.ReadMessage(&reply);
```

### RTMP 推流与拉流

```cpp
// 服务端：publish 时读音视频，play 时往连接上写。examples/rtmp 是一个直播转发。
RtmpServer server([](RtmpConn &conn, const RtmpRequest &req) {
    if (!req.publish) return COCO_SUCCESS;
    RtmpMessage msg;
    while (conn.ReadMessage(&msg) == COCO_SUCCESS) {
        // msg.type：RTMP_MSG_AUDIO / RTMP_MSG_VIDEO / RTMP_MSG_DATA_AMF0
    }
    return COCO_SUCCESS;
});
server.ListenAndServe("0.0.0.0", 1935);   // RTMPS 用 ListenAndServeTLS
```

```cpp
// 客户端。rtmps:// 要先 SetDialer(TlsDialer())。
RtmpClient pub;
pub.Dial("rtmp://127.0.0.1:1935/live/stream");
pub.Publish();
RtmpMessage msg;
msg.type = RTMP_MSG_AUDIO;
msg.timestamp = 0;
msg.payload = "...";
pub.WriteMessage(msg);
```

完整代码见 [`examples/`](examples)。

## 示例程序

示例的地址和端口写死在各自的 `main` 里，不读命令行参数（`ws_client` 可以传 URL，`lookup` 传要解析的主机名，RUDP 的两个示例可以传端口）。

| 程序 | 地址 | 说明 |
| --- | --- | --- |
| `pingpong_server_tcp` / `pingpong_client_tcp` | `127.0.0.1:8080` | TCP 回显，服务端用 `TcpServer` |
| `pingpong_server_udp` / `pingpong_client_udp` | `127.0.0.1:8080` | UDP 回显，服务端直接用 `ListenRoutine` |
| `http_server` / `http_client` | `0.0.0.0:9082` | HTTPS，需在 `examples/http-server/` 下启动以读到证书 |
| `ws_server` / `ws_client` | `0.0.0.0:9083/echo` | WebSocket 回显，也可以用 `websocat ws://127.0.0.1:9083/echo` 测 |
| `rtmp_server` | `0.0.0.0:1935/{app}/{stream}` | RTMP 直播转发，同一路径上一个推流、多个拉流 |
| `lookup` | 系统配置的 DNS 服务器 | 每个主机名一条协程，同时解析，例如 `lookup example.com localhost` |
| `rudp_echo_server` / `rudp_echo_client` | `127.0.0.1:9000`（UDP） | RUDP 回显：服务端是 `TcpServer` + `RudpListener`，客户端回显 1MB 后打印重传次数、SRTT 和拥塞窗口 |

```bash
cd examples/http-server
../../build/bin/http_server
../../build/bin/http_client
```

## 构建

依赖：CMake ≥ 3.14（CMake 4 可用）、C++11 编译器、Git、Perl（OpenSSL 的 `Configure` 需要）。

```bash
# macOS
xcode-select --install && brew install cmake git
# Debian / Ubuntu
sudo apt-get install build-essential cmake git perl
```

`./build.sh` 的常用参数：

| 参数 | 作用 |
| --- | --- |
| `-c` | 先清空 `build/` |
| `-d` | Debug 构建（默认 Release） |
| `-j N` | 并行编译数，默认为 CPU 核数 |
| `-v` | 输出完整的 cmake / make 日志 |
| `--no-examples` | 不编译示例 |
| `-i` | 安装到 `--prefix`，默认是仓库内的 `dist/` |
| `--prefix DIR` | 安装目录 |
| `-t` | 构建后运行 ctest |

不用脚本：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### 在自己的工程里使用

安装之后用 `find_package`：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j && cmake --install build --prefix /opt/coco
```

```cmake
find_package(coco 0.1 REQUIRED)            # -DCMAKE_PREFIX_PATH=/opt/coco
target_link_libraries(app PRIVATE coco::coco)
```

也可以把仓库放进自己的工程，`add_subdirectory(coco)` 后同样链接 `coco::coco`，这时 coco 的示例、测试和 install 规则默认都不生成，不会混进父工程的安装内容。不用 CMake 时：`pkg-config --static --cflags --libs coco`。

默认得到静态库 `libcoco.a`，它依赖的 `libst.a`、`libssl.a`、`libcrypto.a` 装在 `lib/coco/` 下，不会覆盖系统的 OpenSSL，`find_package` 和 pkg-config 会自动带上它们。`-DBUILD_SHARED_LIBS=ON` 生成动态库，`-DCOCO_USE_SYSTEM_OPENSSL=ON` 改用系统的 OpenSSL（例如 macOS 上加 `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`）。AddressSanitizer、产物路径、排错等见 [构建文档](.harness/docs/build.md)。

## 架构

```text
server     HttpServer、RtmpServer（server.*）      协议的 Serve 函数，TcpServer + 可选 TlsHandler
app     |  app/ 下 HTTP、WebSocket、RTMP 的会话     只依赖 StreamConn / StreamDialer
tls     |  net/tls/：TlsConn、TlsDialer、TlsHandler 把一个 StreamConn 包成另一个 StreamConn
net        net/ 的其余部分：接口、TCP、UDP、RUDP、TcpServer、DNS   st_read / st_write / st_accept
codec      各协议的 codec/                          协议本身，不碰连接和协程
core       协程、日志、错误码、工具                 st_thread_create
```

网络代码分两个目录，只看一个问题：它是不是在帮你拿到一条字节流（`StreamConn`）。是的放 `net/`：TCP、UDP、在 UDP 上做出字节流的 RUDP（`net/rudp/`）、`TcpServer`、拨号前要用的 DNS（`net/dns/`），以及把字节流变成加密字节流的 TLS（`net/tls/`）。在字节流上说话的应用层协议放 `app/`，每个协议一个目录，内部再分三层：`codec/` 是协议本身，用一段字节就能读懂和测试；目录里的其余文件是会话，在一条 `StreamConn` 上驱动 codec；`server` 先是逐连接服务的函数（`ServeHttpConn`），再用 `TcpServer` 组装成服务（`HttpServer`），和 Go 的 `net/http/server.go` 一样放在一个文件里。读一个协议就按这个顺序往上看。

`net/tls/` 在分层上比 `net` 的其余部分高一层，`TcpServer`、socket、DNS 都不能用它；`app` 和 `tls` 平级、互不依赖：客户端通过注入的 `StreamDialer` 建连，https / wss 时由调用方传 `TlsDialer()`；服务端的 `TcpServer` 也不认识 TLS，只对每条连接调用处理函数，`HttpServer` / `RtmpServer` 的 `ListenAndServeTLS` 把处理函数换成 `TlsHandler(cfg, …)`：先握手，再把明文连接交给协议，对应 Go 的 `(*conn).serve` 开头那次 `tlsConn.Handshake()`。一个文件只能 include 同一层或更低层的头文件，平级层之间也不能互相 include；codec 不能 include `base/` 和 ST，一个协议的 codec 只给本协议用。ctest 里的 `LayerDependencies` 用例会扫描 `src/` 检查这些规则。因为层与层之间只通过 `StreamConn` 交互，在中间插一层包装就能抓包、注入延迟或截断，而不用改协议代码。

`src/coco/` 目录布局，安装后就是 `include/coco/`（`utils/utils.hpp`、`md5` / `sha1` / `base64`、`base/shutdown.hpp`、`net/rudp/endpoint.hpp` 只在库内部用，不安装）：

```text
src/coco/
├── coco.h           汇总头文件
├── coco_api.h       CocoInit、CocoRun、CocoWaitForShutdown / CocoShutdown、ListenTcp / DialTcp / TcpConnFromFd、ListenUdp / DialUdp、CocoSleepMs、CocoShouldStop
├── base/            协程：CoCoroutine、ListenRoutine、ConnRoutine、ConnManager；TaskGroup；CocoThread；退出与信号
├── common/          错误码
├── log/             日志
├── utils/           IoReader / IoWriter、BufReader、base64 / sha1 / md5
├── net/             拿到字节流：StreamConn 等接口、TCP、UDP、TcpServer
│   ├── dns/         codec/：报文、resolv.conf 与 hosts、应答；resolver：Resolver、LookupHost
│   ├── rudp/        codec/：报文、协议状态机（RudpControl）；endpoint（内部）；conn：RudpConn、RudpListener、DialRudp、RudpDialer
│   └── tls/         config：TlsConfig；conn：TlsConn、TlsDialer、TlsListener、TlsHandler
└── app/             在字节流上说话的应用层协议
    ├── http/        codec/：头部、报文解析与 body 分帧、URL；handler、response_writer、mux、client（HttpClient）、server（ServeHttpConn、HttpServer）
    ├── ws/          codec/：帧、握手；conn（WebSocketConn）、client（WebSocketClient）、handler（WebSocketHandler）
    └── rtmp/        codec/：握手、chunk、AMF0、命令、URL；conn（RtmpConn）、client（RtmpClient）、server（ServeRtmpConn、RtmpServer）
```

## 文档

- [构建](.harness/docs/build.md)：依赖、构建参数、产物、测试、排错
- [架构](.harness/docs/architecture.md)：分层、并发模型、各协议的行为、错误码
- [协程与连接管理](.harness/docs/coroutine.md)：监听协程和连接协程的所有权，连接如何在自己的栈上释放自己
- [State Threads 与 src/coco/base 的实现](.harness/docs/st.md)：ST 的切换、I/O 让出、中断与退出
- [TLS 握手与读写](.harness/docs/tls.md)：用内存 BIO 把 OpenSSL 接进协程 socket
- [协议规划](.harness/docs/protocols.md)：接下来要加的协议、顺序和完成标准
- [RUDP](.harness/docs/rudp.md)：线上格式、状态机、重传与拥塞窗口、关闭规则、端点和泵协程的所有权

## 测试

```bash
./build.sh -t                                # 构建并跑全部用例
cd build && ctest --output-on-failure        # 已构建时直接跑
./build/bin/coco_tests ConnStopDoesNotWait   # 单独跑一个用例
```

测试不依赖外部框架，每个用例是一个独立进程，超时 10 秒。覆盖协程与连接生命周期、`TcpServer` 关停、阻塞式 `ListenAndServe` 与信号退出、TLS、WebSocket 帧编解码和握手、wss、RTMP 握手与推拉流、DNS 报文与解析（对着本机的假 DNS 服务器，不访问外网），RUDP 的状态机（假时钟下的丢包、乱序、超时）和本机连接（经过丢包中继、关停、HTTP 和 TLS over RUDP），以及分层依赖检查。用例会占用 `127.0.0.1` 的 19181–19360 和 19401–19425 端口。

## 平台

| 系统 | 架构 | 事件系统 |
| --- | --- | --- |
| Linux | x86_64 | epoll |
| macOS（≥ 11.0） | x86_64、Apple Silicon | kqueue |

Linux ARM64 目前不能编译：自带的 State Threads（`thirdparty/st`）的 `md.h` 里，aarch64 只有 macOS 分支，Linux 分支没有，会报 `Unknown CPU architecture`。要支持得先在那个子模块里补上。

## 限制

- 单线程：ST 跑在初始化它的线程上（第一次调用 coco 的线程），coco 的对象都不能跨线程使用；要用多核需要多进程。
- TLS 默认不校验对端证书（`SSL_VERIFY_NONE`）。客户端配置可调用 `TlsConfig::EnablePeerVerification()`，按默认 CA 校验证书；`TlsDialer` 会发送 SNI，并在开启校验时核对主机名。
- 不支持 HTTP/2。
- DNS 解析只看 `/etc/hosts` 和 `/etc/resolv.conf` 里的服务器：不走 `nsswitch.conf` 的其他来源（mDNS、LDAP），macOS 上不认 VPN 的分域解析（scoped resolver），也没有 EDNS0 / DNSSEC / DoH。
- WebSocket 单条消息上限 4MB（`MAX_WS_PACKET`）。
- RTMP 单条消息上限 16777215 字节（`kRtmpMaxMessage`）。没有 RTMPE，也不拆 aggregate 消息。
- RUDP 的拥塞控制只是最简 AIMD（没有 pacing），没有路径 MTU 探测（固定 MSS 1200 字节）、保活、加密认证和防反射放大，端点只能在一个线程上用；适合局域网和受控链路，不要直接暴露在公网上。

## 许可证

[MIT](LICENSE)
