# 文档

- [构建](build.md)：依赖、`./build.sh`、产物路径、示例怎么启动。
- [架构](architecture.md)：源码布局、分层、1:N 协程模型和现有协议。
- [协程与连接管理](coroutine.md)：监听协程和连接协程怎么分工，`ConnManager` 为什么不能在连接自己的栈上释放连接。
- [TLS 握手与读写](tls.md)：用内存 BIO 把 OpenSSL 接进协程 socket，握手循环如何同时覆盖 TLS 1.2 和 TLS 1.3。
