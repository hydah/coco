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

# Run an example
cd build
./bin/pingpong_server_tcp 8080
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

- **[Build Instructions](BUILD_INSTRUCTIONS.md)** - Comprehensive build guide for all platforms
- **[Project Analysis](PROJECT_ANALYSIS.md)** - Detailed architecture and design analysis

## Examples

After building, you can run various examples:

```bash
cd build

# TCP echo server
./bin/pingpong_server_tcp 8080

# HTTP server
./bin/http_server 8080

# WebSocket client
./bin/ws_client ws://echo.websocket.org
```

## Platform Support

- ✅ **Linux** (x86_64, ARM64)
- ✅ **macOS** (Intel x86_64, Apple Silicon ARM64)

The library automatically detects and uses the optimal event system:
- **Linux**: epoll
- **macOS**: kqueue

## Dependencies

- CMake (≥ 3.1)
- C++11 compatible compiler
- Git (for submodules)

See [BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md) for detailed dependency installation.

## License

This project is derived from SRS (Simple Realtime Server) and maintains compatibility with its licensing.