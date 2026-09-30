# TLS 握手与读写

`SslConn` 把 OpenSSL 放在一条普通 TCP 连接上面。套接字由 State Threads 管理，OpenSSL 不直接读写文件描述符，只读写两块内存 BIO。协程在「把 TCP 上的字节喂进 `bio_in`」和「把 `bio_out` 里的字节写出 TCP」之间来回切换。TLS 1.2 和 TLS 1.3 共用这一个循环。

代码在 `src/net/tls/coco_ssl.hpp` 和 `src/net/tls/coco_ssl.cpp`。TLS 自成一层，夹在 `layer4` 和 `layer7` 之间：输入一条 `StreamConn`，输出一条 `StreamConn`。服务端的调用点在 `src/server/coco_tcp_server.cpp`：配置了证书的 `TcpServer` 在每条连接的协程上调用 `SslServer::Handshake`，之后才把连接交给处理函数，`HttpServer` 的 HTTPS 就是这样来的。客户端的调用点在 `src/net/layer7/http/coco_http.cpp` 的 `HttpClient::Connect`，调用 `SslClient::Handshake`。

## 为什么是内存 BIO

ST 的套接字是非阻塞的，读不到数据时 `st_read` 让出协程。OpenSSL 默认的套接字 BIO 会自己 `read` / `write`，遇到 `EAGAIN` 只返回 `SSL_ERROR_WANT_READ`，不会去调用 `st_read`，协程也就不会让出，后面的连接全被卡住。

内存 BIO 把这两边拆开：

```text
对端 TCP
    |  st_read / st_write          协程在这里让出
    v
 bio_in   ---->  SSL 记录层  ---->  bio_out
                    |
                    v
              SSL_read / SSL_write 得到明文
```

- `bio_in`：协程从 TCP 读到的密文，经 `BIO_write` 交给 OpenSSL。
- `bio_out`：OpenSSL 要发出的密文。`FlushOutput()` 用 `BIO_get_mem_data` 取出后 `st_write` 写到 TCP，再 `BIO_reset` 清空，避免下次重复发送。

`SSL_set_bio` 把这两块 BIO 交给 `SSL` 对象。之后 `SSL_free` 会一起释放它们，`SslConn` 析构里只 `SSL_free(ssl)` 和 `SSL_CTX_free`。

## 建对象

服务端和客户端共用 `NewSslCtx` 与 `SetupSsl`，差异只有角色：

| | 服务端 `SslServer` | 客户端 `SslClient` |
| --- | --- | --- |
| 角色 | `SSL_set_accept_state` | `SSL_set_connect_state` |
| 证书 | `SSL_use_certificate_file` + `SSL_use_PrivateKey_file` + `SSL_check_private_key` | 不加载 |
| 校验对端 | `SSL_VERIFY_NONE` | `SSL_VERIFY_NONE` |

`SSL_CTX_new(TLS_method())` 让 OpenSSL 自己协商版本，默认能谈到 TLS 1.3，也能回落到 TLS 1.2。加密套件用 `SSL_CTX_set_cipher_list(ctx, "ALL")`。`SSL_MODE_ENABLE_PARTIAL_WRITE` 允许 `SSL_write` 只消化掉明文的前半段，剩余部分由 `SslConn::Write` 的循环继续写。

证书和私钥是 PEM。`SSL_use_PrivateKey_file` 同时接受 RSA 和 EC 私钥。示例证书是 `examples/http-server/server.crt`，2048 位 RSA；OpenSSL 3 默认安全级别拒绝 1024 位的密钥。

`SslConn` 构造时从下层 `TcpConn` 拿走 `st_netfd_t` 的所有权：调用下层的 `Release()` 放弃其 `CocoSocket`，析构时由 `SslConn` 自己关闭 `st_netfd` 并 `delete` 下层对象。

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

    st_read -> buf                 // 让出，直到对端发来数据或超时
    BIO_write(bio_in, buf);
}
```

一次循环里的四种结果：

1. **`r0 == 1`**：握手完成。完成的那一步仍可能往 `bio_out` 放了最后一包（TLS 1.3 服务端的 `Finished`，或随后的 `NewSessionTicket`），所以先 `FlushOutput` 再返回。
2. **`SSL_ERROR_WANT_READ`**：状态机需要更多对端记录。`bio_out` 里可能已经有待发数据（对 `ClientHello` 的回应，或客户端的 `Finished`），先flush，再 `st_read`。
3. **`st_read` 失败**：对端关闭、超时或网络错误，原样返回。超时要在握手前设置。`HttpClient::Connect` 在 `Handshake()` 之前调用 `SetRecvTimeout` / `SetSendTimeout`。服务端由 `TcpServer` 按 `TcpServerOptions` 设置，时机在包好 `SslServer` 之后、握手之前；`SslServer` 会在同一个 fd 上新建自己的 `CocoSocket`，在 `TcpConn` 上设的超时不会带过去。`HttpServer` 把接收超时设为 `HTTP_RECV_TIMEOUT_US`。设晚了，或者 `TcpServerOptions` 保持默认的不超时，握手阶段的 `st_read` 会一直等下去。
4. **其他 `SSL_get_error`**：证书、私钥或协议错误，返回 `ERROR_HTTPS_HANDSHAKE`。

`bio_in` 在整个握手期间不 `reset`。`BIO_write` 追加数据，OpenSSL 自己消费。一次 `st_read` 可能读到多条 TLS 记录，甚至读到握手后的应用数据；清掉 `bio_in` 会把还没解析的字节丢掉，随后 `SSL_read` 就看不到这半条请求。

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
    st_read -> 密文;
    BIO_write(bio_in);
    继续 SSL_read;
}
其他               ERROR_HTTPS_READ
```

写：

```text
对剩余明文循环 SSL_write；
每次成功后 FlushOutput()，把 bio_out 里的密文写到 TCP；
*nwrite 写成已交给 OpenSSL 的明文总字节数。
```

`*nwrite` 由 `SslConn` 从 0 累加后写入，调用方传入的值不参与累加。`Writev` 对每个 `iovec` 调用 `Write`，总字节数同样从 0 算起。

`SSL_read` 返回正数只表示至少有 1 字节明文，不保证填满缓冲区。要读满指定长度用 `ReadFully`，它走下层 `st_read_fully`，读的是密文长度而不是明文长度，不能用来「读满 N 字节 HTTP 正文」。正文长度由 HTTP 层解析 `Content-Length` 后多次 `Read` 凑齐。

## 数据进了哪一层

一条 HTTPS 请求在服务端经过的对象：

```text
TcpServer::Acceptor::Cycle
  Accept()                         ListenRoutine 协程，st_accept
  new Session -> Start()           新的 ConnRoutine 协程
      Session::DoCycle
        new SslServer(stfd, tcp)   接管 TcpConn 的 fd
        SetRecvTimeout
        SslServer::Handshake       DoHandshake 循环
        handler = ServeHttpConn    TcpServer 的处理函数，只看到明文 StreamConn
          HttpMessage::Parse(conn) 多次 SslConn::Read，得到明文 HTTP
          mux 写响应               SslConn::Write
      Cycle 返回
      delete Session               仍在这条协程上，析构里关闭 st_netfd
        ConnManager::Remove        从存活名单里移除
```

明文只出现在 `SSL_read` / `SSL_write` 的缓冲区。TCP 上的字节始终是 TLS 记录，日志里的 `handshake done, TLSv1.3` 来自 `SSL_get_version`。
