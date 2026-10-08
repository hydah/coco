# coco

English | [中文](README.md)

coco is a C++11 networking library built on [State Threads](https://github.com/hydah/state-threads) (ST). Every connection runs in its own coroutine and you write plain synchronous code: when `Read` has no data, the current coroutine yields, the event loop runs other coroutines, and control comes back once data arrives. No callbacks, no hand-written state machines.

It supports TCP, UDP, TLS, HTTP/1.1, WebSocket and RTMP, resolves host names with its own DNS resolver inside the coroutine, and runs on Linux (epoll) and macOS (kqueue).

## Features

- **Synchronous style, asynchronous execution**: one kernel thread per process running many coroutines, 64KB stack each by default.
- **Protocols**: TCP / UDP, TLS 1.2 / 1.3 (server and client), HTTP/1.1 (keep-alive, chunked, routing), WebSocket (`ws://` and `wss://`, server and client), RTMP (`rtmp://` and `rtmps://`, publish and play).
- **Name resolution that doesn't stall the thread**: `DialTcp` and every client use the bundled DNS resolver (reads `/etc/resolv.conf` and `/etc/hosts`, asks for A and AAAA together, caches by TTL). Waiting for an answer only suspends the calling coroutine, where `getaddrinfo` would stop the whole thread.
- **Layered by protocol**: one directory per layer, each depending only on the layers below. HTTP and WebSocket only see a `StreamConn` and don't care whether TCP or TLS is underneath. The layering rule is enforced by a test.
- **Managed connection lifecycle**: `TcpServer` handles accept, the TLS handshake, connection cleanup and shutdown. You only write a handler function.
- **Error codes, not exceptions**: every call returns an `int`; `COCO_SUCCESS` is 0.
- **Usable as a library**: one `libcoco`, every name in `namespace coco`, and public headers that don't expose ST, OpenSSL or http-parser; works with `find_package(coco)`, `add_subdirectory` and pkg-config.
- **Bundled dependencies**: ST and http-parser are git submodules; OpenSSL is built from source as a static library.

## Quick start

```bash
git clone --recursive https://github.com/hydah/coco.git
cd coco
./build.sh            # Release build, binaries in build/bin/
./build.sh -t         # build and run the tests
```

If you cloned without submodules, run `git submodule update --init --recursive` first. The first build downloads the OpenSSL source tarball from GitHub; see [.harness/docs/build.md](.harness/docs/build.md) (Chinese) for offline builds.

Run a TCP echo:

```bash
./build/bin/pingpong_server_tcp    # terminal 1, listens on 127.0.0.1:8080
./build/bin/pingpong_client_tcp    # terminal 2
```

## Code examples

### TCP echo server

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
        // Serves until Ctrl-C / SIGTERM, then closes every connection and returns
        return server.ListenAndServe("127.0.0.1", 8080) == COCO_SUCCESS ? 0 : 1;
    });
}
```

`CocoRun` is where the program starts: it sets up the coroutine runtime on the calling thread, then calls the function it was given. Each new connection calls the handler in its own coroutine and is freed automatically when the handler returns. Per-connection state can simply live in local variables.

`coco/coco.h` includes the whole public API. Every name is in `namespace coco` (the snippets below leave out `coco::`); macros start with `COCO_` or `coco_`.

### Runtime and shutdown

A program starts with `return CocoRun([]() { ... });` and makes every coco call inside it. `CocoRun` sets up ST on the calling thread, returns the error code right away if that fails, and makes the body of the program stoppable with Ctrl-C (see the next section). Without `CocoRun` things still work: the first call that needs a coroutine or a socket sets up ST, and `CocoInit()` checks the setup on its own if you want that.

Every thread that uses coco has a runtime of its own, set up on first use. What coco creates may only be used on the thread that created it; the "Threads" section below shows how several threads work together.

`ListenAndServe` / `ListenAndServeTLS` / `Serve` block until `Stop()` or a shutdown request (`SIGINT`, `SIGTERM` or `CocoShutdown()`). Before returning they close the listening port and wait for every connection to exit, so `main` can simply return. To run several servers, or do other work besides serving:

```cpp
HttpServer api(&mux);
TcpServer echo(Echo);
api.Start("0.0.0.0", 8080);    // Start / StartTLS return once serving has started
echo.Start("0.0.0.0", 9000);
CocoWaitForShutdown();         // until Ctrl-C; each server stops in its destructor
```

A handler must not call `Stop()`, which waits for every connection including its own; to end the program it calls `CocoShutdown()`.

Signal rules: the first `SIGINT` / `SIGTERM` requests a graceful shutdown and hands the signals back to whatever handled them before; a second one always ends the process, even if shutting down hangs, or some code never yields so the first one was never served. A signal that was ignored at startup (the `SIGINT` of a background job in a shell) stays ignored.

### Making the main loop stoppable too

A server shuts down gracefully through the blocking `ListenAndServe`. When you write the main loop yourself (a client that keeps reading and writing, say), `CocoRun` is also what stops it: it calls `fn` and returns what it returns. When a shutdown is requested while `fn` runs, `CocoShouldStop()` turns true and the blocking call in progress fails once, so the loop ends by itself and the objects on `fn`'s stack are destroyed as usual.

```cpp
int main() {
    return CocoRun([]() {
        std::unique_ptr<TcpConn> conn;
        if (DialTcp("127.0.0.1", 8080, 1000 * 1000, &conn) != COCO_SUCCESS) return 1;
        while (!CocoShouldStop()) {      // true after Ctrl-C, SIGTERM or CocoShutdown()
            // conn->Write(), conn->Read(), CocoSleepMs() ...
        }
        return 0;                        // conn is closed here
    });
}
```

`fn` runs on the main coroutine, on the process's own stack rather than a 64KB coroutine stack. Only the blocking call in progress fails; later ones work, so the loop has to check `CocoShouldStop()`. The two pingpong clients in `examples/pingpong` are written this way.

### Several things at once on one thread

`TaskGroup` runs functions side by side on the calling thread, each on a coroutine of its own, and cancels and waits for them together. There is nothing to derive from and nothing to delete: its destructor cancels the functions and waits until they have returned, so they may use what lives on the stack of the scope that owns the group.

```cpp
return CocoRun([&]() {
    TaskGroup tasks;
    tasks.Spawn([&]() { return ReadLoop(conn); });
    tasks.Spawn([&]() { return Heartbeat(conn); });
    return tasks.Wait();         // on Ctrl-C the body is interrupted, and Wait cancels the group, then waits
});
```

- `Cancel()` interrupts every function of the group without waiting; one spawned afterwards starts interrupted.
- `Wait()` only suspends the calling coroutine and returns the first error a function returned. When the caller itself is interrupted (or `CocoShouldStop()` is true for it), `Wait()` cancels the group first and goes on waiting, so a stop request reaches the functions below.
- A function that ignores the interrupt makes `Wait()`, and the destructor, wait for ever.

Scheduling is cooperative: while a coroutine neither blocks nor yields, nothing else on its thread gets in, not the other coroutines, not I/O, and not a stop request, which `CocoShouldStop()` cannot see either when it comes from another thread. A loop that does no I/O calls `CocoYield()` every few milliseconds, then checks `CocoShouldStop()`; work that really burns the CPU goes to a thread of its own (see `CocoThread::Call()` below), not to one that serves I/O.

### Threads

All coroutines of one thread share one core. The simplest way to use more is to let a server hand its connections to worker threads:

```cpp
TcpServerOptions opt;
opt.threads = 4;                 // this thread only accepts; each connection goes to the worker with the fewest
TcpServer server(Echo, opt);     // HttpServer takes HttpServeOptions::threads
return server.ListenAndServe("0.0.0.0", 8080);
```

The handler is then called on several threads at once, so whatever it shares must be safe to use from all of them (a lock or an atomic). A pthread lock that waits holds up every coroutine of its thread, so keep critical sections short. For TLS wrap the handler: `TcpServer server(TlsHandler(cfg, Echo), opt)` (or call `HttpServer::ListenAndServeTLS`); the handshake runs on the worker, on the connection's own coroutine. `Stop()` and a shutdown request make each worker interrupt its connections, wait for them and end. `RtmpServer` is single-threaded for now: a publisher and its players have to be on one thread to relay.

`CocoThread` is the building block, a kernel thread with a runtime of its own:

```cpp
CocoThread worker;               // CocoThread worker(1000): at most 1000 functions not returned yet
worker.Start();
worker.Post([]() { /* runs on a new coroutine on the worker, and may block on I/O */ });
long n = 0;
int ret = worker.Call([&]() { n = Compute(); return COCO_SUCCESS; });  // waits for it, returns what it returned
worker.Stop();                   // interrupts what still runs, waits for it, joins the thread
```

`Post()` and `Call()` may be called from any thread; everything else only on the thread that created the object.

- **`Post()`** never blocks, and its success only means the function is queued. Posted functions each run on a coroutine of their own, side by side, not one after the other.
- **`Call()`** suspends the calling coroutine (or blocks a thread without a runtime) until the function has returned, and returns what it returned. The function may use the caller's stack, so an interrupt of the caller does not end the wait. Each call takes a pipe: use it for a request now and then, not for every message.
- **Load limit**: with a limit given to the constructor, `Post()` and `Call()` fail with `ERROR_THREAD_BUSY` while that many functions have not returned.

Like a server's handler, a posted function has to end when it is interrupted (a blocking call fails once and `CocoShouldStop()` turns true). `Stop()` interrupts it, and so does a shutdown request, functions posted after the request included: they start interrupted, as a `CocoRun` body started after a shutdown request does. The thread itself runs until `Stop()`.

`Stop()` only suspends the calling coroutine; the other coroutines of its thread keep running. It has no timeout: it waits for ever for a function that ignores the interrupt. Functions posted to a thread that never started are destroyed by `Stop()` without running.

Only two things move between threads:

- **Functions**, through `CocoThread::Post()` and `Call()`. What a function captures moves with it, but what coco made (sockets, connections) still belongs to the thread that made it.
- **Raw fds**: `TcpConn::Release()` gives up the fd without closing it, and another thread wraps it into its own runtime with `TcpConnFromFd()`. A TLS session cannot change threads, so wrap the `TlsConn` after the move.

Which to use: `CocoRun` for work on one thread; the `threads` option for a server on several cores; `CocoThread` to hand tasks or connections to another thread; plain `std::thread`s, each with its own `CocoRun`, for threads that share nothing. `threads_single`, `threads_server`, `threads_post` and `threads_plain` in `examples/threads/` show the four, printing the thread each step runs on.

`CocoShutdown()` and `CocoShutdownRequested()` are process-wide: any thread may call them, one coco knows nothing about included, and every thread's waiters and `CocoRun` get the request. Apart from that, connections, listeners, servers, `HttpClient` and `WebSocketClient` must stay on their thread; a build without `NDEBUG` aborts when a coroutine, a socket or a `CocoThread` is used from another one. The default client of `HttpGet` / `HttpPost` is one per thread.

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
// Client: modeled after Go; wss:// needs a TLS dialer injected first
WebSocketClient ws;
ws.SetTlsDialer(TlsDialer());
ws.Dial("ws://127.0.0.1:9083/echo");
ws.Send("hello");
std::string reply;
ws.ReadMessage(&reply);
```

### RTMP publish and play

```cpp
// Server: read media on publish, write it on play. examples/rtmp is a live relay.
RtmpServer server([](RtmpConn &conn, const RtmpRequest &req) {
    if (!req.publish) return COCO_SUCCESS;
    RtmpMessage msg;
    while (conn.ReadMessage(&msg) == COCO_SUCCESS) {
        // msg.type: RTMP_MSG_AUDIO / RTMP_MSG_VIDEO / RTMP_MSG_DATA_AMF0
    }
    return COCO_SUCCESS;
});
server.ListenAndServe("0.0.0.0", 1935);   // ListenAndServeTLS for RTMPS
```

```cpp
// Client. rtmps:// needs SetDialer(TlsDialer()) first.
RtmpClient pub;
pub.Dial("rtmp://127.0.0.1:1935/live/stream");
pub.Publish();
RtmpMessage msg;
msg.type = RTMP_MSG_AUDIO;
msg.timestamp = 0;
msg.payload = "...";
pub.WriteMessage(msg);
```

Full sources are in [`examples/`](examples).

## Example programs

Addresses and ports are hardcoded in each `main`; command-line arguments are ignored (except `ws_client`, which accepts a URL, and `lookup`, which takes the host names to resolve).

| Program | Address | Notes |
| --- | --- | --- |
| `pingpong_server_tcp` / `pingpong_client_tcp` | `127.0.0.1:8080` | TCP echo; the server uses `TcpServer` |
| `pingpong_server_udp` / `pingpong_client_udp` | `127.0.0.1:8080` | UDP echo; the server uses `ListenRoutine` directly |
| `http_server` / `http_client` | `0.0.0.0:9082` | HTTPS; start from `examples/http-server/` so the certificate is found |
| `ws_server` / `ws_client` | `0.0.0.0:9083/echo` | WebSocket echo; `websocat ws://127.0.0.1:9083/echo` works too |
| `lookup` | the system's DNS servers | resolves each host name on a coroutine of its own, all at once, e.g. `lookup example.com localhost` |
| `rtmp_server` | `0.0.0.0:1935/{app}/{stream}` | RTMP live relay: one publisher and any number of players on the same path |

```bash
cd examples/http-server
../../build/bin/http_server
../../build/bin/http_client
```

## Building

Requirements: CMake ≥ 3.14 (CMake 4 works), a C++11 compiler, Git, and Perl (needed by OpenSSL's `Configure`).

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
| `-i` | Install into `--prefix`, `dist/` inside the repository by default |
| `--prefix DIR` | Install directory |
| `-t` | Run ctest after building |

Without the script:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Using coco in your project

After installing, use `find_package`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j && cmake --install build --prefix /opt/coco
```

```cmake
find_package(coco 0.1 REQUIRED)            # -DCMAKE_PREFIX_PATH=/opt/coco
target_link_libraries(app PRIVATE coco::coco)
```

You can also vendor the repository and `add_subdirectory(coco)`, then link `coco::coco` the same way; coco's examples, tests and install rules are all off by default then, so nothing of it ends up in the parent project's install. Without CMake: `pkg-config --static --cflags --libs coco`.

The default is a static `libcoco.a`. The `libst.a`, `libssl.a` and `libcrypto.a` it needs are installed under `lib/coco/`, so they never shadow a system OpenSSL, and `find_package` and pkg-config add them for you. `-DBUILD_SHARED_LIBS=ON` builds a shared library; `-DCOCO_USE_SYSTEM_OPENSSL=ON` uses the system OpenSSL instead (on macOS add e.g. `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`). See [.harness/docs/build.md](.harness/docs/build.md) for AddressSanitizer, output paths and troubleshooting.

## Architecture

```text
server     HttpServer, RtmpServer (server.*)            the protocol's Serve function; TcpServer + optional TlsHandler
app     |  HTTP, WebSocket, RTMP sessions in app/       depend only on StreamConn / StreamDialer
tls     |  net/tls/: TlsConn, TlsDialer, TlsHandler     wraps one StreamConn into another
net        the rest of net/: interfaces, TCP, UDP, TcpServer, DNS resolver   st_read / st_write / st_accept
codec      each protocol's codec/                       the protocol itself, no connection, no coroutines
core       coroutines, log, errors, utils               st_thread_create
```

Networking code is split by one question: does it help you get a byte stream (`StreamConn`)? If so it is in `net/`: TCP, UDP, `TcpServer`, the DNS resolver that dialing needs (`net/dns/`), and TLS, which turns one byte stream into an encrypted one (`net/tls/`). The application protocols that talk over a byte stream are in `app/`, one directory each, layered inside: `codec/` is the protocol itself, readable and testable with plain bytes; the other files are the session, which drives the codec over one `StreamConn`; `server` holds the per-connection function (`ServeHttpConn`) and the service that `TcpServer` makes of it (`HttpServer`), in one file like Go's `net/http/server.go`. Read a protocol in that order, bottom up.

`net/tls/` is one layer above the rest of `net`, so `TcpServer`, sockets and DNS cannot use it; `app` and `tls` are siblings and don't depend on each other: clients open connections through an injected `StreamDialer`; for https / wss the caller passes `TlsDialer()`. On the server side `TcpServer` doesn't know TLS either, it only calls the handler for each connection; `ListenAndServeTLS` of `HttpServer` / `RtmpServer` wraps that handler in `TlsHandler(cfg, ...)`, which handshakes and then hands the plaintext connection to the protocol, like the `tlsConn.Handshake()` at the start of Go's `(*conn).serve`. A file may only include headers from its own layer or lower, and sibling layers may not include each other; a codec may not include `base/` or ST, and a protocol's codec is for that protocol only. The `LayerDependencies` ctest case scans `src/` to enforce these rules. Because layers talk to each other only through `StreamConn`, you can insert a wrapper between any two layers to capture bytes, inject delays or truncate data without touching protocol code.

Source layout under `src/coco/`, installed as `include/coco/` (`utils/utils.hpp`, `md5` / `sha1` / `base64` and `base/shutdown.hpp` are internal and not installed):

```text
src/coco/
├── coco.h           umbrella header
├── coco_api.h       CocoInit, CocoRun, CocoWaitForShutdown / CocoShutdown, ListenTcp / DialTcp / TcpConnFromFd, ListenUdp / DialUdp, CocoSleepMs, CocoShouldStop
├── base/            coroutines: CoCoroutine, ListenRoutine, ConnRoutine, ConnManager; TaskGroup; CocoThread; shutdown and signals
├── common/          error codes
├── log/             logging
├── utils/           IoReader / IoWriter, BufReader, base64 / sha1 / md5
├── net/             getting a byte stream: StreamConn etc. interfaces, TCP, UDP, TcpServer
│   ├── dns/         codec/: messages, resolv.conf and hosts, answers; resolver: Resolver, LookupHost
│   └── tls/         config: TlsConfig; conn: TlsConn, TlsDialer, TlsListener, TlsHandler
└── app/             application protocols over a byte stream
    ├── http/        codec/: headers, message parsing and body framing, URLs; handler, response_writer, mux, client (HttpClient), server (ServeHttpConn, HttpServer)
    ├── ws/          codec/: frames, handshake; conn (WebSocketConn), client (WebSocketClient), handler (WebSocketHandler)
    └── rtmp/        codec/: handshake, chunks, AMF0, commands, URLs; conn (RtmpConn), client (RtmpClient), server (ServeRtmpConn, RtmpServer)
```

## Documentation

The design documents are written in Chinese:

- [Build](.harness/docs/build.md): dependencies, build options, outputs, tests, troubleshooting
- [Architecture](.harness/docs/architecture.md): layering, concurrency model, protocol behavior, error codes
- [Coroutines and connection management](.harness/docs/coroutine.md): who owns listener and connection coroutines, and how a connection frees itself on its own stack
- [State Threads and src/coco/base](.harness/docs/st.md): ST context switching, I/O yielding, interruption and exit
- [TLS handshake and I/O](.harness/docs/tls.md): plugging OpenSSL into coroutine sockets with memory BIOs
- [Protocol roadmap](.harness/docs/protocols.md): the protocols to add next, in what order, and when each counts as done

## Tests

```bash
./build.sh -t                                # build and run all cases
cd build && ctest --output-on-failure        # if already built
./build/bin/coco_tests ConnStopDoesNotWait   # run a single case
```

The tests need no external framework. Each case runs as its own process with a 10-second timeout. They cover coroutine and connection lifecycles, `TcpServer` shutdown, the blocking `ListenAndServe` and signal shutdown, TLS, WebSocket framing and handshakes, wss, the RTMP handshake and publish/play, DNS messages and resolution (against a fake name server on the loopback, never the internet), and the layer dependency check. Cases listen on `127.0.0.1` ports 19181–19360.

## Platforms

| OS | Architecture | Event system |
| --- | --- | --- |
| Linux | x86_64 | epoll |
| macOS (≥ 11.0) | x86_64, Apple Silicon | kqueue |

Linux ARM64 does not build at the moment: in the bundled State Threads (`thirdparty/st`), `md.h` handles aarch64 only in its macOS branch, not in the Linux one, and stops with `Unknown CPU architecture`. Supporting it means adding that to the submodule first.

## Limitations

- Single-threaded: ST runs on the thread that set it up (the first one to call into coco), and coco objects must not cross threads; use multiple processes to use multiple cores.
- TLS does not verify the peer certificate unless the client config calls `TlsConfig::EnablePeerVerification()` (default CA store). `TlsDialer` sends SNI, and checks the hostname when verification is on.
- No HTTP/2.
- DNS resolution only uses `/etc/hosts` and the servers in `/etc/resolv.conf`: no other `nsswitch.conf` sources (mDNS, LDAP), no macOS scoped resolvers (VPN split DNS), and no EDNS0 / DNSSEC / DoH.
- A WebSocket message is capped at 4MB (`MAX_WS_PACKET`).
- An RTMP message is capped at 16777215 bytes (`kRtmpMaxMessage`). No RTMPE, and aggregate messages are not unpacked.

## License

[MIT](LICENSE)
