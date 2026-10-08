# 协议规划

现在支持的协议见 [架构](architecture.md) 的“协议”一节：TCP、UDP、RUDP、TLS、HTTP/1.1（服务端和客户端）、WebSocket、RTMP，以及在 UDP 上做的 DNS 解析。这份文档记录接下来按什么顺序加哪些协议、每个落在哪一层、做到什么程度算完成。做完一项，把它挪到架构文档里，这里只留一句“已完成”和链接。

排序的原则：先修影响已有功能的缺口，再沿着已有的方向（RTMP + HTTP 的流媒体服务）补齐，最后才是需要新传输层或新并发模型的大项。

## 总览

| 顺序 | 协议 | 层 | 状态 |
| --- | --- | --- | --- |
| 1 | DNS 解析 | net | 已完成 |
| 2 | HTTP-FLV | app | 未开始 |
| 3 | HLS | app | 未开始 |
| 4 | TLS ALPN + HTTP/2 | tls、app | 未开始 |
| 5 | RTSP / RTP | app | 按需 |
| 6 | RESP（Redis）客户端 | app | 按需 |
| 7 | SRT、QUIC | net | 暂不做 |
| 8 | RUDP（最小可靠字节流） | net | 已完成，见 [RUDP](rudp.md) |

## 1. DNS 解析（已完成）

原来 `DialStream` / `DialDatagram` 调用 `getaddrinfo`。它是阻塞的系统调用，解析期间整条 ST 线程停住，这个线程上所有协程都跟着卡；`HttpClient`、`WebSocketClient`、`RtmpClient` 和 `DialTcp` / `DialUdp` 都经过这里。

现在由 `src/coco/net/dns/` 里的解析器接管，查询走协程化的 UDP，不再阻塞线程。设计和行为见架构文档的“DNS”一条。没有做的：EDNS0、DNSSEC、DoT / DoH、`nsswitch.conf` 里 files 和 dns 以外的来源（mDNS、LDAP）、IDN。

## 2. HTTP-FLV

把 RTMP 收到的音视频消息封装成 FLV（9 字节文件头 + 每条消息一个 tag + PreviousTagSize），用一条不设 Content-Length 的 HTTP 响应持续写出。HTTP 服务端已经支持 `Flush()` 后转 chunked，所以不需要新的传输。

- 位置：`src/coco/app/flv/`。FLV 头和 tag 的读写不碰连接，放 `flv/codec/`，路径本身就让分层检查把它当 codec；把 RTMP 消息写成 HTTP 响应的转发放在 `flv/` 的会话文件或 `flv/server.*`，在 `check_layers.cmake` 的 `COCO_PROTO_ALLOWED` 里加 `flv:rtmp`、`flv:http`。`rtmp` 和 `http` 之间仍互不 include。
- 要点：新观众先收到 metadata、音视频 sequence header，再从最近的关键帧开始；慢观众要有发送队列上限，超了就断开，不能拖住推流协程。
- 完成标准：`examples/rtmp` 能推流后用 ffplay / flv.js 播放；单测覆盖 FLV 编码、从关键帧开始、慢消费者被断开。

## 3. HLS

把流切成 TS（或 fMP4）分片，维护一个滑动的 m3u8 列表，由 HTTP 服务端按文件或内存提供。

- 位置：TS 封装（PAT / PMT / PES）放 `app/hls/codec/`，切片策略按关键帧和目标时长。
- 要点：分片在内存里还是落盘要可选；列表更新和分片写入要原子，播放器不能读到半个分片。
- 完成标准：Safari / hls.js 能播放；单测覆盖 TS 包的 CRC、PCR、连续计数和 m3u8 内容。

## 4. TLS ALPN + HTTP/2

先在 `TlsConfig` 加 ALPN 协议列表，`TlsConn` 暴露协商结果；这一步本身很小，也是 HTTP/2 的前提。

HTTP/2 本身工作量最大：帧编解码、HPACK（含 Huffman）、流状态机、连接级和流级流量控制、SETTINGS / PING / GOAWAY。难点在并发模型：现在一条连接一条协程、读写同步，而 HTTP/2 一条连接上同时有多个流。打算用一条读协程把帧分发给各个流，每个流的处理函数在自己的协程里（用 `TaskGroup` 管理），写出统一走一把锁，沿用 WebSocket “读协程 + 发送锁”的做法。`HttpHandler` 接口保持不变，处理函数不需要知道底下是 1.1 还是 2。

- 位置：`app/http2/`，允许依赖 `http`（复用 `HttpRequest`、`HttpHandler`）。
- 完成标准：h2spec 通过；`curl --http2` 和浏览器能访问；`HttpClient` 能在 https 上协商 h2。h2c（明文升级）不做。

## 5. RTSP / RTP（按需）

监控摄像头接入常用。RTSP 是文本信令，形式接近 HTTP；RTP 走 UDP（`UdpConn` 已有）或在 RTSP 的 TCP 连接里交织。等有接入摄像头的需求再做。

## 6. RESP 客户端（按需）

协议很简单，价值在验证“`StreamDialer` + 连接池”能否从 `HttpClient` 里抽出来给别的客户端复用。抽出来之后再考虑 MQTT。

## 7. SRT、QUIC（暂不做）

这两个都是 UDP 上自己做可靠传输和拥塞控制，相当于一套新的传输层（net），HTTP/3 还要求 QUIC 的 TLS 1.3 集成方式（OpenSSL 3.5 有 QUIC API，但接法和现在的内存 BIO 不同）。没有明确的低延迟推流或 HTTP/3 需求之前不做。

## 8. RUDP（已完成）

UDP 上有序、可靠、带流量控制和最简 AIMD 拥塞控制的字节流，行为摘要见 [架构](architecture.md) 的“协议”一节，设计和测试见 [RUDP](rudp.md)。不改变上面对 SRT、QUIC 的判断。

## 每一项的共同要求

- 编解码和会话分开：编解码放 `codec/`，不碰连接和协程，单测可以不起网络。
- 只依赖 `StreamConn` / `DatagramConn` / `StreamDialer`，不直接碰 fd 和 OpenSSL。
- 阻塞只能发生在 ST 的 I/O 上；任何可能阻塞线程的系统调用（像原来的 `getaddrinfo`）都不允许出现在协程路径上。
- 按 [测试](../testing.md) 跑完全部环境，并同步改架构文档、README 和示例。
