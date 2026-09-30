# 设计文档

- [协程与连接管理](coroutine.md)：State Threads 上的协程模型、监听/连接分工，以及 `ConnManager` 为什么不能在连接自己的栈上释放连接。
- [TLS 握手与读写](tls.md)：用内存 BIO 把 OpenSSL 接进协程 socket，握手循环如何同时覆盖 TLS 1.2 和 TLS 1.3。
