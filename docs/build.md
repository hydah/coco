# 构建

coco 用 CMake 构建。State Threads 和 http-parser 是 git submodule，OpenSSL 在配置阶段从源码编成静态库。

## 环境

- CMake 3.5 或更高（CMake 4 可以）
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
| `-i` | 编译完成后 `make install`，产物在仓库里的 `dist/` |
| `-t` | 预留的测试开关。仓库里没有测试目标，运行后只会提示还没有配置测试 |

不用脚本时：

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

不传 `CMAKE_BUILD_TYPE` 时，`CMakeLists.txt` 自己会设成 Debug。`./build.sh` 则会显式传入 Release。

## 产物

| 路径 | 内容 |
| --- | --- |
| `lib/libcoco.a` | coco |
| `lib/libst.a` | State Threads |
| `thirdparty/temp/out_libs/openssl-<version>/lib/` | `libssl.a`、`libcrypto.a` |
| `build/bin/` | 示例程序 |

OpenSSL 版本写在 `cmake/openssl.cmake`（当前 3.5.9）。第一次配置会从 GitHub 下载源码包并校验 SHA256，只编译库，不编译 OpenSSL 自带的测试和文档。离线构建时把对应的 `openssl-<version>.tar.gz` 放到 `thirdparty/`，CMake 会用本地文件，不再下载。`thirdparty/temp/` 和 `thirdparty/openssl-*.tar.gz` 都不进版本库。

Linux 上事件系统是 epoll，OpenSSL 用上游的 `./config` 探测本机。macOS 上事件系统是 kqueue。

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

## 排错

**submodule 是空的。** `thirdparty/st` 或 `thirdparty/http-parser` 没有文件时，先执行 `git submodule update --init --recursive`。

**CMake 报版本太低。** 最低要求是 3.5。CMake 4 不再接受 `cmake_minimum_required` 低于 3.5 的声明。

**OpenSSL 下载失败。** 把 `cmake/openssl.cmake` 里写明版本和 SHA256 的源码包放到 `thirdparty/openssl-<version>.tar.gz` 再配置。

**改了 CMake 却还是旧选项。** `./build.sh -c`，或者删掉 `build/` 后重新配置。只删 `build/` 不会重编 OpenSSL；要重编 OpenSSL 再删 `thirdparty/temp/`。
