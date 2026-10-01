# coco

English | [中文](README.md)

coco is a C++11 networking library built on [State Threads](https://github.com/hydah/state-threads) (ST). Every connection runs in its own coroutine and you write plain synchronous code: when `Read` has no data, the current coroutine yields, the event loop runs other coroutines, and control comes back once data arrives. No callbacks, no hand-written state machines.

It supports TCP, UDP, TLS, HTTP/1.1 and WebSocket on Linux (epoll) and macOS (kqueue).

## Features

- **Synchronous style, asynchronous execution**: one kernel thread per process running many coroutines, 64KB stack each by default.
- **Protocols**: TCP / UDP, TLS 1.2 / 1.3 (server and client), HTTP/1.1 (keep-alive, chunked, routing), WebSocket (`ws://` and `wss://`, server and client).
- **Layered by protocol**: one directory and one static library per layer, each depending only on the layers below. HTTP and WebSocket only see a `StreamConn` and don't care whether TCP or TLS is underneath. The layering rule is enforced by a test.
- **Managed connection lifecycle**: `TcpServer` handles accept, the TLS handshake, connection cleanup and shutdown. You only write a handler function.
- **Error codes, not exceptions**: every call returns an `int`; `COCO_SUCCESS` is 0.
- **Bundled dependencies**: ST and http-parser are git submodules; OpenSSL is built from source as a static library.

## Quick start

```bash
git clone --recursive https://github.com/hydah/coco.git
cd coco
./build.sh            # Release build, binaries in build/bin/
./build.sh -t         # build and run the tests
```

If you cloned without submodules, run `git submodule update --init --recursive` first. The first build downloads the OpenSSL source tarball from GitHub; see [docs/build.md](docs/build.md) (Chinese) for offline builds.

Run a TCP echo:

```bash
./build/bin/pingpong_server_tcp    # terminal 1, listens on 127.0.0.1:8080
./build/bin/pingpong_client_tcp    # terminal 2
```

## Code examples

### TCP echo server

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

    CocoLoopMs(1000);   // the main coroutine drives the event loop here
    return 0;
}
```

Each new connection calls the handler in its own coroutine and is freed automatically when the handler returns. Per-connection state can simply live in local variables.

### HTTP / HTTPS server

```cpp
HttpServeMux mux;
// Go 1.22 ServeMux patterns: optional method, {name} segments, a {path...} tail, /static/ for a subtree
mux.HandleFunc("GET /hello/{name}", [](HttpResponseWriter &w, HttpRequest &r) {
    w.Write("hello " + r.PathValue("name"));   // small responses get Content-Length and go out in one write
});
mux.HandleFunc("POST /echo", [](HttpResponseWriter &w, HttpRequest &r) {
    std::string body;
    r.body.ReadAll(&body);
    w.Header().Set(HttpHeaderContentType, HttpContentTypeJson);
    w.Write(body);
});

HttpServer server(&mux);
server.ListenAndServe("0.0.0.0", 8080);
// HTTPS: server.ListenAndServeTLS("0.0.0.0", 9082, "server.crt", "server.key");
CocoLoopMs(1000);
```

The client is modeled after Go's `http.Client`: keep-alive connections are pooled per host and redirects are followed. HTTPS needs `TlsDialer()` injected:

```cpp
HttpClient client;
client.SetTlsDialer(TlsDialer());   // not needed for http:// only

std::unique_ptr<HttpResponse> resp;
if (client.Get("https://127.0.0.1:9082/hello/coco", &resp) == COCO_SUCCESS) {
    std::string body;
    resp->body.ReadAll(&body);   // the connection returns to the pool when resp is destroyed
}

HttpRequest req("PUT", "http://127.0.0.1:8080/items/1", "{\"n\":1}");
req.header.Set("Content-Type", "application/json");
client.Do(req, &resp);
// A shared default client: HttpGet(url, &resp), HttpPost(url, type, body, &resp)
```

### WebSocket server and client

```cpp
// Server: the handler is the connection's lifetime; CLOSE 1000 is sent when it returns
HttpServeMux mux;
mux.Handle("/echo", new WebSocketHandler([](WebSocketConn *ws) {
    std::string data;
    WebSocketHeader::Type type;
    while (ws->ReadMessage(&data, &type) == COCO_SUCCESS) {
        ws->Send(data, type);
    }
}));
HttpServer server(&mux);   // ListenAndServeTLS for wss
server.ListenAndServe("0.0.0.0", 9083);
```

```cpp
// Client: modeled after Go; wss:// needs a TLS dialer injected first (net/tls/coco_tls.hpp)
WebSocketClient ws;
ws.SetTlsDialer(TlsDialer());
ws.Dial("ws://127.0.0.1:9083/echo");
ws.Send("hello");
std::string reply;
ws.ReadMessage(&reply);
```

Full sources are in [`examples/`](examples).

## Example programs

Addresses and ports are hardcoded in each `main`; command-line arguments are ignored (except `ws_client`, which accepts a URL).

| Program | Address | Notes |
| --- | --- | --- |
| `pingpong_server_tcp` / `pingpong_client_tcp` | `127.0.0.1:8080` | TCP echo; the server uses `TcpServer` |
| `pingpong_server_udp` / `pingpong_client_udp` | `127.0.0.1:8080` | UDP echo; the server uses `ListenRoutine` directly |
| `http_server` / `http_client` | `0.0.0.0:9082` | HTTPS; start from `examples/http-server/` so the certificate is found |
| `ws_server` / `ws_client` | `0.0.0.0:9083/echo` | WebSocket echo; `websocat ws://127.0.0.1:9083/echo` works too |

```bash
cd examples/http-server
../../build/bin/http_server
../../build/bin/http_client
```

## Building

Requirements: CMake ≥ 3.5 (CMake 4 works), a C++11 compiler, Git, and Perl (needed by OpenSSL's `Configure`).

```bash
# macOS
xcode-select --install && brew install cmake git
# Debian / Ubuntu
sudo apt-get install build-essential cmake git perl
```

Common `./build.sh` options:

| Option | Effect |
| --- | --- |
| `-c` | Remove `build/` first |
| `-d` | Debug build (default is Release) |
| `-j N` | Parallel jobs, defaults to the CPU count |
| `-v` | Print full cmake / make output |
| `--no-examples` | Skip the examples |
| `-i` | Install into `dist/` inside the repository |
| `-t` | Run ctest after building |

Without the script:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

In your own CMake project, link the `coco` target to get every layer plus ST and OpenSSL; a TCP-only program can link just `coco_l4`, and plain HTTP / WebSocket needs only `coco_l7`, without OpenSSL. See [docs/build.md](docs/build.md) for AddressSanitizer, output paths and troubleshooting.

## Architecture

```text
server     TcpServer, HttpServer                    accept loop + optional TLS + protocol handler
layer7  |  HTTP, WebSocket                          depends only on StreamConn / StreamDialer
tls     |  TlsConn, TlsListener, TlsDialer          wraps one StreamConn into another
layer4     StreamConn etc. interfaces; TcpConn, UdpConn   st_read / st_write / st_accept
core       coroutines, log, errors, utils           st_thread_create
```

`layer7` and `tls` are siblings and don't depend on each other: clients open connections through an injected `StreamDialer`, `TlsDialer()` for https / wss, so plain HTTP / WebSocket programs don't need OpenSSL. A file may only include headers from its own layer or lower, and sibling layers may not include each other. The `LayerDependencies` ctest case scans `src/` to enforce this. Because layers talk to each other only through `StreamConn`, you can insert a wrapper between any two layers to capture bytes, inject delays or truncate data without touching protocol code.

Source layout:

```text
src/
├── coco_api.h       CocoInit, ListenTcp / DialTcp, ListenUdp / DialUdp, CocoSleepMs, CocoShouldStop
├── base/            coroutines: CoCoroutine, ListenRoutine, ConnRoutine, ConnManager
├── common/          error codes
├── log/             logging
├── utils/           IoReader / IoWriter, BufReader, base64 / sha1 / md5
├── net/
│   ├── layer4/      TCP, UDP
│   ├── tls/         TlsConfig, TlsConn, TlsListener, TlsDialer
│   └── layer7/
│       ├── http/    messages, HttpServeMux, ServeHttpConn, HttpClient
│       └── ws/      frame codec, WebSocketConn, WebSocketClient, WebSocketHandler
└── server/          TcpServer, HttpServer
```

## Documentation

The design documents are written in Chinese:

- [Build](docs/build.md): dependencies, build options, outputs, tests, troubleshooting
- [Architecture](docs/architecture.md): layering, concurrency model, protocol behavior, error codes
- [Coroutines and connection management](docs/coroutine.md): who owns listener and connection coroutines, and how a connection frees itself on its own stack
- [State Threads and src/base](docs/st.md): ST context switching, I/O yielding, interruption and exit
- [TLS handshake and I/O](docs/tls.md): plugging OpenSSL into coroutine sockets with memory BIOs

## Tests

```bash
./build.sh -t                                # build and run all cases
cd build && ctest --output-on-failure        # if already built
./build/bin/coco_tests ConnStopDoesNotWait   # run a single case
```

The tests need no external framework. Each case runs as its own process with a 10-second timeout. They cover coroutine and connection lifecycles, `TcpServer` shutdown, TLS, WebSocket framing and handshakes, wss, and the layer dependency check. Cases listen on `127.0.0.1` ports 19181–19228.

## Platforms

| OS | Architecture | Event system |
| --- | --- | --- |
| Linux | x86_64, ARM64 | epoll |
| macOS (≥ 11.0) | x86_64, Apple Silicon | kqueue |

## Limitations

- Single-threaded: ST runs on the thread that called `CocoInit()`; use multiple processes to use multiple cores.
- TLS does not verify the peer certificate (`SSL_VERIFY_NONE`).
- No HTTP/2.
- A WebSocket message is capped at 4MB (`MAX_WS_PACKET`).

## License

[MIT](LICENSE)
