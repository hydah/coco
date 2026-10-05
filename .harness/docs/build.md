# 构建

coco 用 CMake 构建。State Threads 和 http-parser 是 git submodule，OpenSSL 在配置阶段从源码编成静态库。

## 环境

- CMake 3.14 或更高（CMake 4 可以）
- C++11 编译器：macOS 上用 Xcode Command Line Tools，Linux 上用 gcc 或 clang
- Git
- Perl（OpenSSL 的 `Configure` 是 Perl 脚本）
- 首次构建需要能访问 GitHub，用来拉取 submodule 和 OpenSSL 源码包

macOS：

```bash
xcode-select --install
brew install cmake git
```

Debian/Ubuntu：

```bash
sudo apt-get install build-essential cmake git perl
```

部署目标是 macOS 11.0。构建产物是本机架构，不生成 universal binary。Apple Silicon 上应使用 arm64 的 Command Line Tools，直接 `./build.sh`。只有工具链本身是 x86_64 时，才需要 `arch -x86_64 ./build.sh`，让 `build.sh` 把 CMake 配成 x86_64。

## 构建

```bash
git submodule update --init --recursive
chmod +x build.sh
./build.sh
```

`./build.sh` 默认是 Release。常用参数：

| 参数 | 作用 |
| --- | --- |
| `-c` | 先删掉 `build/` |
| `-d` | Debug（默认是 Release） |
| `-j N` | 并行编译数，默认按 CPU 核数 |
| `-v` | 把 cmake/make 的输出打到终端，而不是 `build/` 下的日志 |
| `--no-examples` | 不编译 `examples/` |
| `-i` | 编译完成后 `make install`，装到 `--prefix` |
| `--prefix DIR` | 安装目录，默认是仓库里的 `dist/` |
| `-t` | 编译完成后在 `build/` 里跑 `ctest`，有失败时脚本以非零退出 |

不用脚本时：

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

不传 `CMAKE_BUILD_TYPE` 时，`CMakeLists.txt` 自己会设成 Debug。`./build.sh` 则会显式传入 Release。

CMake 选项：

| 选项 | 默认 | 作用 |
| --- | --- | --- |
| `BUILD_SHARED_LIBS` | `OFF` | 生成 `libcoco.so` / `libcoco.dylib`，ST 和 OpenSSL 链接进去 |
| `COCO_USE_SYSTEM_OPENSSL` | `OFF` | 用 `find_package(OpenSSL)` 找到的 OpenSSL，不从源码编译；位置不标准时加 `-DOPENSSL_ROOT_DIR=...`。macOS 上链接 Homebrew 的 OpenSSL 时，链接器会提示它比部署目标 11.0 新，不影响使用，要去掉就把 `CMAKE_OSX_DEPLOYMENT_TARGET` 设成 Homebrew 的版本 |
| `COCO_BUILD_EXAMPLES` / `COCO_BUILD_TESTS` | 顶层构建 `ON`，作为子项目 `OFF` | 编译示例 / 测试 |
| `COCO_INSTALL` | 顶层构建 `ON`，作为子项目 `OFF` | 生成库的 install 规则；作为子项目时由父工程决定安装什么 |
| `COCO_INSTALL_EXAMPLES` | `OFF` | 同时把示例程序装到 `bin/examples/<目录>/`，`http_server` 的证书和私钥装在它旁边 |
| `COCO_ENABLE_ASAN` | `OFF` | AddressSanitizer，见下文 |

编译选项（`-Wall`、C++11、平台宏）都挂在 coco 自己的 target 上，作为子项目时不会改动父工程的全局设置。

动态库不做符号可见性控制，所有符号都导出，包括 ST、http-parser，以及自带 OpenSSL 时的 `SSL_*` / `EVP_*`。在 Linux 上，进程里如果还有另一份 OpenSSL，这些符号可能和它冲突，所以动态库建议配 `COCO_USE_SYSTEM_OPENSSL=ON`；静态库不受影响。测试直接调用 ST 的函数，所以动态库构建下它们依赖这种导出。

## 产物

构建目录里（除 OpenSSL 外都不写进源码树）：

| 路径 | 内容 |
| --- | --- |
| `build/lib/libcoco.a` | coco，一个库包含全部层，见 [架构](architecture.md) 的“分层” |
| `build/lib/libst.a` | State Threads |
| `build/bin/` | 示例程序、`coco_tests` |
| `thirdparty/temp/out_libs/openssl-<version>-<hash>/lib/` | `libssl.a`、`libcrypto.a`，`<hash>` 由编译选项算出 |

安装后（`cmake --install build --prefix <dir>` 或 `./build.sh -i`）：

| 路径 | 内容 |
| --- | --- |
| `include/coco/` | 公共头文件，入口是 `coco/coco.h` |
| `lib/libcoco.a` | coco |
| `lib/coco/libst.a`、`libssl.a`、`libcrypto.a` | 静态 `libcoco.a` 依赖的库；放在子目录里，不会覆盖系统的 OpenSSL。动态库或系统 OpenSSL 时没有对应文件 |
| `lib/cmake/coco/` | `find_package(coco)` 用的配置，导出 `coco::coco` |
| `lib/pkgconfig/coco.pc` | pkg-config，静态链接时用 `pkg-config --static --libs coco` |

在 CMake 里链接 `coco::coco` 就够了，它会带上 include 目录、C++11 要求，以及静态链接时需要的 `st`、OpenSSL、pthread、dl。三种用法：

```cmake
# 1. 安装后：cmake -DCMAKE_PREFIX_PATH=<dir> ...
find_package(coco 0.1 REQUIRED)
# 2. 源码放进自己的工程
add_subdirectory(third_party/coco)

target_link_libraries(app PRIVATE coco::coco)
```

不用 CMake 时：`c++ -std=c++11 main.cpp $(pkg-config --static --cflags --libs coco)`。

公共头文件不 include `st.h`、OpenSSL 或 http-parser 的头文件，所以使用方不需要它们的 include 路径。需要直接操作 `TlsConfig::ctx()` 时，自己 include `<openssl/ssl.h>`。

OpenSSL 版本写在 `cmake/openssl.cmake`（当前 3.5.9）。第一次配置会从 GitHub 下载源码包并校验 SHA256，只编译库，不编译 OpenSSL 自带的测试和文档。编好的库放在 `thirdparty/temp/` 下，所有构建目录共用，按编译选项分目录，所以 Debug / Release、静态 / 动态、顶层 / 子项目这些构建不会互相触发重编。离线构建时把对应的 `openssl-<version>.tar.gz` 放到 `thirdparty/`，CMake 会用本地文件，不再下载。`thirdparty/temp/` 和 `thirdparty/openssl-*.tar.gz` 都不进版本库。

Linux 上事件系统是 epoll，OpenSSL 用上游的 `./config` 探测本机。macOS 上事件系统是 kqueue。

## 测试

测试在 `tests/`，不依赖外部测试框架。ctest 把每个用例注册成单独的进程，超时 10 秒，所以一个用例崩溃或卡住只会算在它自己头上。用例会在 `127.0.0.1` 上监听 19181–19315 端口。

`LayerDependencies` 不是 C++ 用例，它用 `cmake -P` 运行 `cmake/check_layers.cmake`，检查 `src/` 下没有向上层的 include。也可以单独跑：`cmake -DSRC_DIR=src -P cmake/check_layers.cmake`。

```bash
./build.sh -t                               # Release 构建并跑测试
cd build && ctest --output-on-failure       # 已经构建过时直接跑
./build/bin/coco_tests ConnStopDoesNotWait  # 单独跑一个用例；不带参数则在一个进程里跑全部
```

新增用例时，除了在源文件里用 `COTEST(Name)` 定义，还要把名字加进 `tests/CMakeLists.txt` 的 `COCO_TEST_CASES`。

排查内存问题时可以开 AddressSanitizer。ST 会自己切换栈，所以要关掉“栈返回后使用”检测。用 RelWithDebInfo（`-O2 -g`）而不是 Debug：`-O0` 加上 ASan 的栈帧比 64KB 的协程栈还大，会报 stack-overflow：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCOCO_ENABLE_ASAN=ON -DCOCO_BUILD_EXAMPLES=OFF
cmake --build build-asan -j --target coco_tests
cd build-asan && ASAN_OPTIONS=detect_stack_use_after_return=0 ctest --output-on-failure
```

`-DCOCO_BUILD_TESTS=OFF` 可以不编译测试。

## 跑示例

示例的地址和端口写在各自的 `main` 里，不读命令行参数。

```bash
# 终端 1
./build/bin/pingpong_server_tcp
# 终端 2，连 127.0.0.1:8080
./build/bin/pingpong_client_tcp
```

UDP 的一对是 `pingpong_server_udp` / `pingpong_client_udp`，端口同样是 8080。

HTTPS 示例从当前目录读取 `./server.key` 和 `./server.crt`，要在 `examples/http-server/` 下启动：

```bash
cd examples/http-server
../../build/bin/http_server          # 0.0.0.0:9082
../../build/bin/http_client          # 请求 https://127.0.0.1:9082/
```

启动日志里会有 `st_set_eventsys to kqueue`（macOS）或 `st_set_eventsys to epoll`（Linux）。

`examples/threads/` 下的四个程序对照几种用法。每个都自己起服务器、自己当客户端，跑完自动退出，输出里每行标出所在的线程（T0 是主线程）：

| 程序 | 用法 | 能看到什么 |
| --- | --- | --- |
| `threads_single` | 主线程 `CocoRun` | 三个各要 100ms 的回复一共约 100ms，全在 T0 |
| `threads_server` | `TcpServerOptions::threads = 4` | T0 只 accept，8 个连接轮流分到 T1～T4 |
| `threads_post` | `TaskGroup` + `CocoThread::Call` | 300ms 计算放在主线程会让 tick 停 300ms；用 `Call` 交给 worker 后 tick 照常，只有调用的那条协程等结果 |
| `threads_plain` | `std::thread` + 各自 `CocoRun` | 两个线程各跑一个服务器，主线程一次 `CocoShutdown()` 把两个都停掉 |

```bash
./build/bin/threads_post
```

## 排错

**submodule 是空的。** `thirdparty/st` 或 `thirdparty/http-parser` 没有文件时，先执行 `git submodule update --init --recursive`。

**CMake 报版本太低。** 最低要求是 3.14。

**OpenSSL 下载失败。** 把 `cmake/openssl.cmake` 里写明版本和 SHA256 的源码包放到 `thirdparty/openssl-<version>.tar.gz` 再配置。

**改了 CMake 却还是旧选项。** `./build.sh -c`，或者删掉 `build/` 后重新配置。只删 `build/` 不会重编 OpenSSL；要重编 OpenSSL 再删 `thirdparty/temp/`。

**`find_package(coco)` 找不到 OpenSSL。** 用 `COCO_USE_SYSTEM_OPENSSL=ON` 构建的 coco 安装后会 `find_dependency(OpenSSL)`，使用方也要能找到同一份 OpenSSL，例如同样传 `-DOPENSSL_ROOT_DIR`。
