# 文档

- [编码与审查](../development.md)：实现前的行为推演、按改动选择的风险、定向回归和反例审查。
- [构建](build.md)：依赖、`./build.sh`、产物路径、示例怎么启动。
- [架构](architecture.md)：源码布局、分层、1:N 协程模型和现有协议。
- [协程与连接管理](coroutine.md)：监听协程和连接协程归谁所有，连接为什么可以在自己的栈上释放自己，`ConnManager` 怎样等所有连接退出。
- [State Threads 与 src/coco/base 的实现](st.md)：ST 的创建、切换、I/O 让出、中断和退出，`Cycle()` 的调用链，资源释放的两条路径，以及为什么还需要 `ConnManager`。
- [TLS 握手与读写](tls.md)：用内存 BIO 把 OpenSSL 接进协程 socket，握手循环如何同时覆盖 TLS 1.2 和 TLS 1.3。
- [协议规划](protocols.md)：接下来要加的协议、顺序、各自落在哪一层和完成标准；DNS 已完成。
- [测试要求](../testing.md)：macOS、Linux 和不同架构都要跑完的检查；`.harness/scripts/` 里的脚本把它们自动跑完。
