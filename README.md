# Coco Networking Library

An async event library based on state threads (st), inspired by SRS (Simple Realtime Server).

## Features

- **Coroutine-based concurrency** using state threads
- **High-performance networking** with epoll (Linux) and kqueue (macOS)
- **Multiple protocol support**: TCP, UDP, HTTP/1.1, WebSocket, SSL/TLS
- **Cross-platform**: Linux and macOS (Intel & Apple Silicon)
- **Easy-to-use API** with synchronous-style programming

## Quick Start

### Using the Build Script (Recommended)

```bash
# Clone the repository
git clone <repository-url>
cd coco

# Build the project
chmod +x build.sh
./build.sh

# Run an example (port is hardcoded to 8080)
./build/bin/pingpong_server_tcp
```

### Manual Build

```bash
git submodule update --init --recursive
mkdir build && cd build
cmake ..
make -j$(nproc)  # Linux
# or
make -j$(sysctl -n hw.ncpu)  # macOS
```

## Documentation

- **[构建](docs/build.md)**
- **[架构](docs/architecture.md)**
- **[协程与连接管理](docs/coroutine.md)**
- **[TLS 握手与读写](docs/tls.md)**

## Examples

Ports and addresses are hardcoded in each example. HTTPS reads `./server.key` and `./server.crt` from the working directory.

```bash
# TCP echo, 127.0.0.1:8080
./build/bin/pingpong_server_tcp
./build/bin/pingpong_client_tcp

# HTTPS, run from examples/http-server so the certificate is found
cd examples/http-server
../../build/bin/http_server
../../build/bin/http_client
```

## Platform Support

- ✅ **Linux** (x86_64, ARM64)
- ✅ **macOS** (Intel x86_64, Apple Silicon ARM64)

The library automatically detects and uses the optimal event system:
- **Linux**: epoll
- **macOS**: kqueue

## Dependencies

- CMake (≥ 3.5)
- C++11 compatible compiler
- Git (for submodules)
- Perl (OpenSSL's configure script)

See [docs/build.md](docs/build.md) for dependency installation and build options.

## License

This project is derived from SRS (Simple Realtime Server) and maintains compatibility with its licensing.