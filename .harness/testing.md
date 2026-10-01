# coco：测试要求

改了构建、运行时、网络、TLS、CMake，或准备说「做完了」之前，测试必须在 **macOS、Linux，以及不同 CPU 架构** 上都跑完。本机是 Apple Silicon，Linux 和另一种架构用容器补，不能只在本机跑一遍 ctest 就停。

本机 Docker 由 colima 提供，架构是 aarch64。容器名和镜像标签都带上架构，例如容器 `coco-linux-amd64`、`coco-linux-arm64`，镜像 `coco-linux:amd64`、`coco-linux:arm64`。这样不同架构可以同时跑，也不会占用已有容器的名字，更不会把 `ubuntu:24.04` 这种已有标签换到另一份镜像上。测完删掉自己起的容器和自己打的标签。

## 要覆盖的环境

| 环境 | 怎么跑 | 期望 |
| --- | --- | --- |
| macOS / Apple Silicon | 本机直接构建 | 全部通过 |
| Linux / x86_64 | `docker run --name coco-linux-amd64 --platform linux/amd64`，基础镜像 Ubuntu 24.04 | 全部通过 |
| Linux / aarch64 | `docker run --name coco-linux-arm64`（colima 原生就是 aarch64），同样 Ubuntu 24.04 | 目前编不过，见下方「已知失败」；每次仍要试一次，确认失败原因没变 |

容器里装：`build-essential` `cmake` `perl` `pkg-config` `libssl-dev`。

往容器里送源码用 `COPYFILE_DISABLE=1 tar --no-xattrs`，不要把 macOS 的 `._*` 资源叉文件打进去。`src/CMakeLists.txt` 会过滤这类文件，但别的工具不会。

## 每个能编过的环境都要做

1. **四种链接组合都编、都测**，不能只测默认的那一种：

   | | 自带 OpenSSL（默认） | 系统 OpenSSL（`-DCOCO_USE_SYSTEM_OPENSSL=ON`） |
   | --- | --- | --- |
   | 静态库（默认） | 要测 | 要测 |
   | 动态库（`-DBUILD_SHARED_LIBS=ON`） | 要测 | 要测 |

   macOS 上系统 OpenSSL 不在默认搜索路径，加 `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`。链接器提示 Homebrew 的库比部署目标 11.0 新，是预期的，不影响使用。

2. **ctest 全部通过**，并且再跑一次 `./bin/coco_tests`（不带参数，所有用例在同一个进程里）。单进程用来发现上一个用例留下的全局状态，比如没复位的退出请求。

3. 改了安装、导出或公共头文件时，装到一个临时前缀，再用两种方式各编一个小程序并跑起来：`find_package(coco)` 链接 `coco::coco`，以及 `pkg-config --static --cflags --libs coco`（动态库去掉 `--static`）。

4. 查内存用 AddressSanitizer 时用 RelWithDebInfo，不要用 Debug：`-O0` 的栈帧比 64KB 协程栈还大，会误报 stack-overflow。

   ```bash
   cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCOCO_ENABLE_ASAN=ON -DCOCO_BUILD_EXAMPLES=OFF
   cmake --build build-asan -j --target coco_tests
   cd build-asan && ASAN_OPTIONS=detect_stack_use_after_return=0 ctest --output-on-failure
   ```

## 容器里的注意点

- **x86_64 是用 QEMU 模拟的**，`cc1` 会随机段错误（`internal compiler error: Segmentation fault`）。构建失败时看是不是这种崩溃：是的话直接重试，`cmake --build` 会从断点续编；重试数次仍失败才算真正的编译错误。
- 用例监听 `127.0.0.1` 上的固定端口。本机不要同时跑两套测试；容器有自己的网络命名空间，和本机一起跑没问题。
- 两个架构的容器名不同，可以同时跑。测完删掉这两个容器和为它们打的镜像标签。

## 已知失败

**Linux aarch64 编不过**，并且失败发生在编 coco 之前。`thirdparty/st/md.h` 里 aarch64 只有 macOS 分支，Linux 分支会报 `Unknown CPU architecture`。这是 State Threads 子模块的限制，不是 coco 本身的回归。要支持得先改那个子模块。

每次在 aarch64 容器里试一次：如果还是这条错误，在结论里写明「已知失败，原因未变」；如果错误变了，或者居然编过了，单独说明，不要把它和别的失败混在一起。

## 结论里要写清

- 每个环境、每种链接组合的测试结果。
- 哪一项没跑，以及原因。
- Linux aarch64 是已知失败，还是出现了新情况。
