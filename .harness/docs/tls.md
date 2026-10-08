# TLS 握手与读写

`TlsConn` 把 OpenSSL 放在任意一条 `StreamConn` 上面，通常是 TCP，也可以是测试里的包装连接。OpenSSL 不直接读写文件描述符，只读写两块内存 BIO。协程在「把下层读到的字节喂进 `bio_in`」和「把 `bio_out` 里的字节写给下层」之间来回切换。TLS 1.2 和 TLS 1.3 共用这一个循环。

代码在 `src/coco/net/tls/`：`config.hpp` / `config.cpp` 是 `TlsConfig`，`conn.hpp` / `conn.cpp` 是其余部分：

- `TlsConfig`：持有一个 `SSL_CTX`，由所有用它建的连接共享。证书和私钥在 `NewServer` 时加载一次，不再每条连接读一次文件。
- `TlsConn`：一条 TLS 连接，拥有或借用下层 `StreamConn`。密文只经过下层的 `Read` / `Write`，超时也原样转给下层。对应 Go 的 `tls.Conn`。
- `TlsHandler(cfg, next)`：一个 `StreamHandler`，在 `TcpServer` 交给它的连接上借用这条连接建服务端 `TlsConn`、握手，成功后把明文连接交给 `next`，失败就返回握手的错误、不调用 `next`。对应 Go 的 `(*conn).serve` 开头那次 `tlsConn.Handshake()`。
- `TlsListener`：包住另一个 `StreamListener`，把它 `Accept` 出来的每条连接包成 `TlsConn`，对应 Go 的 `tls.NewListener`。传给 `TcpServer::Serve` 也能服务 TLS，但不能配合多线程。
- `TlsDialer()`：包住另一个 `StreamDialer`，返回握手完成的 `TlsConn`，对应 Go 的 `tls.Dialer`。不传配置时所有这样的 dialer 共享一个客户端 `TlsConfig`。

TLS 的作用是拿到一条加密的字节流，所以目录放在 `net/tls/` 里；分层上它自成一层，夹在 `net` 的其余部分和 `app` 之间：输入一条 `StreamConn`，输出一条 `StreamConn`。`TcpServer` 在 `net` 里，不认识 TLS，只对每条连接调用处理函数。服务端的调用点在各协议的 `server.cpp`：`HttpServer::StartTLS` / `RtmpServer::StartTLS` 用 `TlsConfig::NewServer` 加载证书，失败就直接返回 `ERROR_HTTPS_KEY_CRT`，成功则用 `TlsHandler(cfg, …)` 套住协议的 `Serve` 函数再交给 `TcpServer`。客户端用 `TlsDialer()`：它先用下层 dialer（默认 `TcpDialer()`）建连，给下层设上这次拨号的超时，再包成 `TlsConn` 并调用 `Handshake()`。`app` 不 include TLS，`HttpClient` 和 `WebSocketClient` 只是调用注入给它们的 `StreamDialer`，https / wss 由调用方传入 `TlsDialer()`。

## 为什么是内存 BIO

ST 的套接字是非阻塞的，读不到数据时 `st_read` 让出协程。OpenSSL 默认的套接字 BIO 会自己 `read` / `write`，遇到 `EAGAIN` 只返回 `SSL_ERROR_WANT_READ`，不会去调用 `st_read`，协程也就不会让出，后面的连接全被卡住。

内存 BIO 把这两边拆开：

```text
下层 StreamConn（通常是 TCP）
    |  Read / Write                协程在这里让出
    v
 bio_in   ---->  SSL 记录层  ---->  bio_out
                    |
                    v
              SSL_read / SSL_write 得到明文
```

- `bio_in`：协程从下层读到的密文，经 `BIO_write` 交给 OpenSSL。
- `bio_out`：OpenSSL 要发出的密文。`FlushOutput()` 用 `BIO_read` 把它取到 `TlsConn` 自己的缓冲里，再写给下层。取出即消费，不会重复发送。

`SSL_set_bio` 把这两块 BIO 交给 `SSL` 对象。之后 `SSL_free` 会一起释放它们，`TlsConn` 析构里只 `SSL_free(ssl_)`。`SSL_CTX` 属于 `TlsConfig`，最后一个引用它的连接释放后才释放。

## 建对象

服务端和客户端共用 `NewSslCtx`，差异只在 `TlsConfig` 和角色：

| | 服务端 `TlsConfig::NewServer` | 客户端 `TlsConfig::NewClient` |
| --- | --- | --- |
| 角色 | `SSL_set_accept_state` | `SSL_set_connect_state` |
| 证书 | `SSL_CTX_use_certificate_chain_file` + `SSL_CTX_use_PrivateKey_file` + `SSL_CTX_check_private_key` | 不加载 |
| 校验对端 | `SSL_VERIFY_NONE` | `SSL_VERIFY_NONE` |

`TlsConn` 用哪个角色由 `TlsConfig::IsServer()` 决定。证书在 `NewServer` 里就检查完，文件缺失或密钥不匹配时它返回 `ERROR_HTTPS_KEY_CRT`，`TcpServer::Start`（以及 `ListenAndServe`）随之失败，不会等到每次握手才报错。

`SSL_CTX_new(TLS_method())` 让 OpenSSL 自己协商版本，默认能谈到 TLS 1.3，也能回落到 TLS 1.2。加密套件用 `SSL_CTX_set_cipher_list(ctx, "ALL")`。`SSL_MODE_ENABLE_PARTIAL_WRITE` 允许 `SSL_write` 只消化掉明文的前半段，剩余部分由 `TlsConn::Write` 的循环继续写。

证书和私钥是 PEM。`SSL_CTX_use_PrivateKey_file` 同时接受 RSA 和 EC 私钥。示例证书是 `examples/http-server/server.crt`，2048 位 RSA；OpenSSL 3 默认安全级别拒绝 1024 位的密钥。

`TlsConn` 通常通过 `std::unique_ptr` 拥有下层连接，析构时先 `SSL_free`，再释放下层，下层关闭自己的 fd。`TlsHandler` 用的是借用下层的构造函数：下层归 `TcpServer` 所有，`TlsConn` 是处理函数栈上的对象，处理函数返回时先析构，`TcpServer` 随后才关闭下层。它不碰 fd，所以下层可以是任何 `StreamConn`。

## 握手在什么时候做

`SSL` 对象和握手都推迟到第一次 `Read`、`Write` 或显式的 `Handshake()`。握手只做一次，结果记在 `handshake_err_` 上，之后每次调用都直接返回它。一条协程在读、另一条在写时，`handshake_lock_` 保证只有先到的那条去握手，另一条等它完成后拿到同一个结果。

`TlsHandler` 一开始就显式 `Handshake()`，`TlsListener::Accept` 只包装、握手推迟到处理函数第一次读写。两种情况下握手都在 `TcpServer` 给这条连接起的协程上：一个连上来却不发 `ClientHello` 的对端只会卡住它自己的协程，不影响监听协程继续 accept。`TcpServer` 先给 TCP 连接设好超时再调用处理函数，`TlsConn` 的超时又直接转给下层，所以握手同样受 `TcpServerOptions` 的超时约束；`Stop()` 中断连接协程时，卡在握手里的读也会失败返回。多线程时处理函数在 worker 线程上运行，`TlsHandler` 的 `TlsConn` 也就建在 worker 上：`TlsConn` 里的 ST 锁属于建它的线程，不能先在监听线程上包好再交接，这也是 `TlsListener` 不能配合多线程的原因。

## 握手循环

`DoHandshake()` 不按「第几包该是 ClientHello」来写。OpenSSL 每调用一次 `SSL_do_handshake` 就推进自己的状态机，然后通过 `SSL_get_error` 说明还缺什么：

```text
while (true) {
    r0 = SSL_do_handshake(ssl);
    r1 = SSL_get_error(ssl, r0);

    FlushOutput();                 // 这一步产生的记录，包括完成时的最后一包

    if (r0 == 1)
        return success;            // 握手完成
    if (r1 != SSL_ERROR_WANT_READ)
        return ERROR_HTTPS_HANDSHAKE;

    under->Read -> buf             // 让出，直到对端发来数据或超时
    BIO_write(bio_in, buf);
}
```

一次循环里的四种结果：

1. **`r0 == 1`**：握手完成。完成的那一步仍可能往 `bio_out` 放了最后一包（TLS 1.3 服务端的 `Finished`，或随后的 `NewSessionTicket`），所以先 `FlushOutput` 再返回。
2. **`SSL_ERROR_WANT_READ`**：状态机需要更多对端记录。`bio_out` 里可能已经有待发数据（对 `ClientHello` 的回应，或客户端的 `Finished`），先 flush，再读下层。
3. **读下层失败**：对端关闭、超时或网络错误，原样返回。超时要在握手前设置。`TlsConn::SetRecvTimeout` / `SetSendTimeout` 直接设到下层，所以设在 `TlsConn` 上还是设在包装前的 `TcpConn` 上效果相同。客户端由 `TlsDialer` 在握手前给下层设超时；服务端由 `TcpServer` 在调用处理函数之前按 `TcpServerOptions` 设置，握手推迟到处理函数第一次读写，所以同样受这个超时约束。`HttpServer` 把接收超时设为 `HTTP_RECV_TIMEOUT_US`。`TcpServerOptions` 保持默认的不超时时，握手阶段的读会一直等下去。
4. **其他 `SSL_get_error`**：证书、私钥或协议错误，返回 `ERROR_HTTPS_HANDSHAKE`。

`bio_in` 在整个握手期间不 `reset`。`BIO_write` 追加数据，OpenSSL 自己消费。一次读可能读到多条 TLS 记录，甚至读到握手后的应用数据；清掉 `bio_in` 会把还没解析的字节丢掉，随后 `SSL_read` 就看不到这半条请求。

## TLS 1.2 和 TLS 1.3 走同一段代码

两条协议的记录往返不同。握手循环不区分它们，差别都在 OpenSSL 内部。

TLS 1.2 全握手，协程观察到的往返：

```text
客户端                         服务端
ClientHello            -->
                       <--    ServerHello
                              Certificate
                              ServerKeyExchange
                              ServerHelloDone
ClientKeyExchange      -->
ChangeCipherSpec
Finished
                       <--    ChangeCipherSpec
                              Finished
```

TLS 1.3：

```text
客户端                         服务端
ClientHello            -->
（带 key_share）
                       <--    ServerHello
                              EncryptedExtensions
                              Certificate
                              CertificateVerify
                              Finished
Finished               -->
（可与应用数据同一飞行）
                       <--    NewSessionTicket   （握手完成后的记录）
```

TLS 1.3 少一轮明文的 `ServerHelloDone` / `ClientKeyExchange`。服务端在写出自己的 `Finished` 时，`SSL_do_handshake` 就已经返回 1，客户端的 `Finished` 是它还在 `WANT_READ` 时生成并 flush 出去的。若按 TLS 1.2 的固定四步写死（读一轮、写一轮、再读一轮，并且只接受 `WANT_READ`），TLS 1.3 会在中途返回 1，被当成握手失败。

`NewSessionTicket` 是握手完成后的记录。它可能和 `Finished` 排在同一次 `FlushOutput` 里发出，也可能要等下一次 `SSL_read` / `SSL_write` 才从 `bio_out` 取走。两条路径都会 flush。

## 握手之后的读写

`Read` 和 `Write` 仍是「调用 OpenSSL，再按它的要求搬密文」。

读：

```text
r0 = SSL_read(明文缓冲);
if (r0 > 0)            得到明文，返回
if (WANT_READ) {
    FlushOutput();     读也可能产生要发的记录，例如 KeyUpdate 的回应
    under->Read -> 密文;
    BIO_write(bio_in);
    继续 SSL_read;
}
其他               ERROR_HTTPS_READ
```

写：

```text
对剩余明文循环 SSL_write；
每次成功后 FlushOutput()，把 bio_out 里的密文写给下层；
*nwrite 写成已交给 OpenSSL 的明文总字节数。
```

`*nwrite` 由 `TlsConn` 从 0 累加后写入，调用方传入的值不参与累加。`Writev` 对每个 `iovec` 调用 `Write`，总字节数同样从 0 算起。

`SSL_read` 返回正数只表示至少有 1 字节明文，不保证填满缓冲区。要读满指定长度用 `ReadFully`，它循环调用 `Read`，直到拿够这么多明文或出错。

## 一个读、多个写

一条 `TlsConn` 可以由一条协程读，同时由其他协程写，WebSocket 客户端就是这样用的。难点在 `bio_out`：`SSL_write` 往里放记录，`SSL_read` 也会（比如 KeyUpdate 的回应），而 flush 在写下层时会让出。

如果 flush 直接从 `BIO_get_mem_data` 返回的指针写，让出期间另一条协程的 `SSL_write` 往 `bio_out` 追加数据，BIO 可能重新分配内存，这个指针就失效了。随后的 `BIO_reset` 还会丢掉别人刚放进去的记录，TCP 上的记录流被破坏，对端解密失败。

所以 `FlushOutput()` 这样做：

- 持有 `flush_lock_`（ST 的互斥量，按先来后到唤醒）再从 `bio_out` 取数据。一条协程在 flush 时，其他协程产生的记录留在 `bio_out` 里，由持锁者在同一轮循环里一并写出，或者等轮到自己时再写。记录上线的顺序因此和产生的顺序一致。
- 用 `BIO_read` 拷到自己的缓冲再写，让出期间 `bio_out` 怎么变都不影响正在写的字节。
- 写失败时把错误记在 `flush_err_` 上。记录流一旦断了就无法恢复，之后所有的写都返回同一个错误。

多条协程同时 `Read` 不支持：`SSL_read` 缺数据时会去读下层喂 `bio_in`，两条读协程会互相抢走对方的密文。

## 数据进了哪一层

一条 HTTPS 请求在服务端经过的对象：

```text
HttpServer::StartTLS
  TlsConfig::NewServer             加载一次证书和私钥
  handler = TlsHandler(cfg, ServeHttpConn…)
  TcpServer::Start(tcp_listener)
TcpServer::Acceptor::Cycle         ListenRoutine 协程
  TcpListener::Accept              st_accept，得到 TcpConn
  new Session -> Start()           新的 ConnRoutine 协程
      Session::DoCycle -> ServeConn
        SetRecvTimeout             设到 TcpConn
        TlsHandler                 栈上 TlsConn tls(&tcp, cfg)，借用 TcpConn
          tls.Handshake()          失败就返回，不调用 ServeHttpConn
          ServeHttpConn(tls)       只看到明文 StreamConn
            ReadHttpRequest(br)    TlsConn::Read，得到明文 HTTP
            handler 写响应         TlsConn::Write
          返回                     TlsConn 析构
      Cycle 返回
      delete Session               仍在这条协程上：TcpConn 关闭 st_netfd
        ConnManager::Remove        从存活名单里移除
```

明文只出现在 `SSL_read` / `SSL_write` 的缓冲区。TCP 上的字节始终是 TLS 记录，日志里的 `handshake done, TLSv1.3` 来自 `SSL_get_version`。
