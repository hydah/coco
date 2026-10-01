# coco

[English](README_en.md) | 中文

coco 是一个基于 [State Threads](https://github.com/hydah/state-threads)（ST）的 C++11 网络库。每条连接跑在自己的协程里，代码按同步方式写：`Read` 没有数据时，当前协程让出，事件循环去跑别的协程，数据到了再切回来。不需要回调，也不需要手写状态机。

支持 TCP、UDP、TLS、HTTP/1.1 和 WebSocket，运行在 Linux（epoll）和 macOS（kqueue）上。

## 特性

- **同步写法，异步执行**：一个进程一个内核线程，上面跑多条协程，默认栈 64KB。
- **协议齐全**：TCP / UDP、TLS 1.2 / 1.3（服务端和客户端）、HTTP/1.1（keep-alive、chunked、路由）、WebSocket（`ws://` 和 `wss://`，服务端和客户端）。
- **按协议分层**：每层一个目录、一个静态库，只能依赖下层。HTTP 和 WebSocket 只认 `StreamConn`，不关心下面是 TCP 还是 TLS。分层规则由测试强制检查。
- **连接生命周期由框架管理**：`TcpServer` 负责 accept、TLS 握手、连接回收和关停，业务只写一个处理函数。
- **错误码而非异常**：所有接口返回 `int`，`COCO_SUCCESS` 为 0。
- **依赖自带**：ST 和 http-parser 是 submodule，OpenSSL 在构建时从源码编成静态库。

## 快速开始

```bash
git clone --recursive https://github.com/hydah/coco.git
cd coco
./build.sh            # Release 构建，产物在 build/bin/
./build.sh -t         # 构建并运行测试
```

已经 clone 过但没拉 submodule 时，先执行 `git submodule update --init --recursive`。第一次构建会从 GitHub 下载 OpenSSL 源码包，离线构建方法见 [构建文档](docs/build.md)。

跑一个 TCP 回显：

```bash
./build/bin/pingpong_server_tcp    # 终端 1，监听 127.0.0.1:8080
./build/bin/pingpong_client_tcp    # 终端 2
```

## 代码示例

### TCP 回显服务

```cpp
#include "coco_api.h"
#include "common/error.hpp"
#include "server/coco_tcp_server.hpp"

int main() {
    CocoInit();

    TcpServer server([](StreamConn &conn) {
        char buf[1024];
        ssize_t n = 0;
        int ret;
        while ((ret = conn.Read(buf, sizeof(buf), &n)) == COCO_SUCCESS) {
            if ((ret = conn.Write(buf, n, nullptr)) != COCO_SUCCESS) break;
        }
        return ret;
    });
    if (server.ListenAndServe("127.0.0.1", 8080) != COCO_SUCCESS) return -1;

    CocoLoopMs(1000);   // 主协程在这里驱动事件循环
    return 0;
}
```

每条新连接都会在一条独立的协程里调用处理函数，函数返回后连接自动释放。连接的状态直接放在局部变量里即可。

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
CocoLoopMs(1000);
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
// 客户端：用法仿照 Go；wss:// 需要先注入 TLS dialer（net/tls/coco_tls.hpp）
WebSocketClient ws;
ws.SetTlsDialer(TlsDialer());
ws.Dial("ws://127.0.0.1:9083/echo");
ws.Send("hello");
std::string reply;
ws.ReadMessage(&reply);
```

完整代码见 [`examples/`](examples)。

## 示例程序

示例的地址和端口写死在各自的 `main` 里，不读命令行参数（`ws_client` 除外，可以传 URL）。

| 程序 | 地址 | 说明 |
| --- | --- | --- |
| `pingpong_server_tcp` / `pingpong_client_tcp` | `127.0.0.1:8080` | TCP 回显，服务端用 `TcpServer` |
| `pingpong_server_udp` / `pingpong_client_udp` | `127.0.0.1:8080` | UDP 回显，服务端直接用 `ListenRoutine` |
| `http_server` / `http_client` | `0.0.0.0:9082` | HTTPS，需在 `examples/http-server/` 下启动以读到证书 |
| `ws_server` / `ws_client` | `0.0.0.0:9083/echo` | WebSocket 回显，也可以用 `websocat ws://127.0.0.1:9083/echo` 测 |

```bash
cd examples/http-server
../../build/bin/http_server
../../build/bin/http_client
```

## 构建

依赖：CMake ≥ 3.5（CMake 4 可用）、C++11 编译器、Git、Perl（OpenSSL 的 `Configure` 需要）。

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
| `-i` | 安装到仓库内的 `dist/` |
| `-t` | 构建后运行 ctest |

不用脚本：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

在自己的 CMake 工程里链接 `coco` 目标即可拿到全部层和 ST、OpenSSL；只写 TCP 程序时可以只链接 `coco_l4`，只用明文 HTTP / WebSocket 时链接 `coco_l7`，不需要 OpenSSL。AddressSanitizer、产物路径、排错等见 [构建文档](docs/build.md)。

## 架构

```text
server     TcpServer、HttpServer               accept 循环 + 可选 TLS + 协议处理函数
layer7  |  HTTP、WebSocket                     只依赖 StreamConn / StreamDialer
tls     |  TlsConn、TlsListener、TlsDialer      把一个 StreamConn 包成另一个 StreamConn
layer4     StreamConn 等接口；TcpConn、UdpConn  st_read / st_write / st_accept
core       协程、日志、错误码、工具             st_thread_create
```

`layer7` 和 `tls` 平级、互不依赖：客户端通过注入的 `StreamDialer` 建连，https / wss 时传 `TlsDialer()`，所以只用明文 HTTP / WebSocket 时不需要链接 OpenSSL。一个文件只能 include 同一层或更低层的头文件，平级层之间也不能互相 include，ctest 里的 `LayerDependencies` 用例会扫描 `src/` 检查这一点。因为层与层之间只通过 `StreamConn` 交互，在中间插一层包装就能抓包、注入延迟或截断，而不用改协议代码。

`src/` 目录布局：

```text
src/
├── coco_api.h       CocoInit、ListenTcp / DialTcp、ListenUdp / DialUdp、CocoSleepMs、CocoShouldStop
├── base/            协程：CoCoroutine、ListenRoutine、ConnRoutine、ConnManager
├── common/          错误码
├── log/             日志
├── utils/           IoReader / IoWriter、BufReader、base64 / sha1 / md5
├── net/
│   ├── layer4/      TCP、UDP
│   ├── tls/         TlsConfig、TlsConn、TlsListener、TlsDialer
│   └── layer7/
│       ├── http/    报文、HttpServeMux、ServeHttpConn、HttpClient
│       └── ws/      帧编解码、WebSocketConn、WebSocketClient、WebSocketHandler
└── server/          TcpServer、HttpServer
```

## 文档

- [构建](docs/build.md)：依赖、构建参数、产物、测试、排错
- [架构](docs/architecture.md)：分层、并发模型、各协议的行为、错误码
- [协程与连接管理](docs/coroutine.md)：监听协程和连接协程的所有权，连接如何在自己的栈上释放自己
- [State Threads 与 src/base 的实现](docs/st.md)：ST 的切换、I/O 让出、中断与退出
- [TLS 握手与读写](docs/tls.md)：用内存 BIO 把 OpenSSL 接进协程 socket

## 测试

```bash
./build.sh -t                                # 构建并跑全部用例
cd build && ctest --output-on-failure        # 已构建时直接跑
./build/bin/coco_tests ConnStopDoesNotWait   # 单独跑一个用例
```

测试不依赖外部框架，每个用例是一个独立进程，超时 10 秒。覆盖协程与连接生命周期、`TcpServer` 关停、TLS、WebSocket 帧编解码和握手、wss，以及分层依赖检查。用例会占用 `127.0.0.1` 的 19181–19228 端口。

## 平台

| 系统 | 架构 | 事件系统 |
| --- | --- | --- |
| Linux | x86_64、ARM64 | epoll |
| macOS（≥ 11.0） | x86_64、Apple Silicon | kqueue |

## 限制

- 单线程：ST 跑在调用 `CocoInit()` 的线程上，要用多核需要多进程。
- TLS 不校验对端证书（`SSL_VERIFY_NONE`）。
- 不支持 HTTP/2。
- WebSocket 单条消息上限 4MB（`MAX_WS_PACKET`）。

## 许可证

[MIT](LICENSE)
