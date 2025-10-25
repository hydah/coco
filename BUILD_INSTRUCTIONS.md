# Coco Networking Library - Build Instructions

## Overview

This document provides comprehensive build instructions for the Coco networking library on different platforms, with special focus on macOS/ARM support.

## Quick Start

The easiest way to build the project is using the provided build script:

```bash
# Make the script executable and run it
chmod +x build.sh
./build.sh
```

## Supported Platforms

- **Linux** (x86_64, ARM64)
- **macOS** (Intel x86_64, Apple Silicon ARM64)

## Prerequisites

### macOS

Required tools:
- Xcode Command Line Tools
- CMake (≥ 3.1)
- Git

Install with Homebrew:
```bash
brew install cmake git
```

Or install Xcode Command Line Tools:
```bash
xcode-select --install
```

#### Architecture Notes for Apple Silicon (M1/M2)

The build system automatically detects your architecture configuration and handles complex scenarios:

**Automatic Detection Modes:**
- **Native ARM64**: Running natively with ARM64 development tools
- **Rosetta 2 (x86_64)**: Running under Rosetta 2 translation layer
- **Mixed environments**: Native ARM64 execution with x86_64-only development tools

**The build script will:**
1. Detect your current execution architecture
2. Test available compilers and determine their architecture
3. Configure the build for the appropriate architecture
4. Provide clear warnings about architecture mismatches
5. Automatically set correct CMake flags for your environment

**Common Apple Silicon Scenarios:**

1. **Native ARM64 with x86_64 tools** (Most common):
   ```
   Platform: macos (arm64 (x86_64 tools))
   Warning: ARM64 system with x86_64 development tools detected
   The build will be configured for x86_64 architecture
   ```

2. **Running under Rosetta 2**:
   ```
   Platform: macos (arm64 (running under Rosetta 2))
   Note: Running under Rosetta 2 translation layer
   Native architecture: arm64, Current architecture: x86_64
   ```

3. **Native ARM64 with ARM64 tools**:
   ```
   Platform: macos (arm64)
   ```

**Architecture Compatibility Issues:**

If you encounter architecture-related build failures, the build script provides several solutions:

1. **Automatic handling**: The script detects mismatches and configures appropriately
2. **Manual Rosetta mode**: `arch -x86_64 ./build.sh`
3. **Install native tools**: `xcode-select --install` for ARM64 Xcode tools
4. **Homebrew ARM64 tools**: Install ARM64 compilers via Homebrew

The build system is designed to work seamlessly regardless of your development tool architecture, providing optimal compatibility across all Apple Silicon configurations.

### Linux (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install build-essential cmake git
```

### Linux (CentOS/RHEL/Fedora)

```bash
sudo yum install gcc-c++ cmake git
# or for newer versions:
sudo dnf install gcc-c++ cmake git
```

## Build Script Usage

The `build.sh` script provides a comprehensive build system with platform detection and automatic configuration.

### Basic Usage

```bash
# Standard build
./build.sh

# Clean build
./build.sh -c

# Debug build
./build.sh -d

# Verbose output
./build.sh -v
```

### Advanced Options

```bash
# Clean debug build with verbose output
./build.sh -c -d -v

# Build with specific number of parallel jobs
./build.sh -j 8

# Build without examples
./build.sh --no-examples

# Full build with testing and installation
./build.sh -c -t -i
```

### Command Line Options

| Option | Long Form | Description |
|--------|-----------|-------------|
| `-h` | `--help` | Show help message |
| `-c` | `--clean` | Clean build directory before building |
| `-d` | `--debug` | Build in debug mode (default: Release) |
| `-j N` | `--jobs N` | Number of parallel jobs (default: auto-detect) |
| `-t` | `--test` | Run tests after building |
| `-i` | `--install` | Install after building |
| `-v` | `--verbose` | Verbose output |
| | `--no-examples` | Don't build examples |

## Manual Build (Without Script)

If you prefer to build manually:

```bash
# 1. Initialize submodules
git submodule update --init --recursive

# 2. Create build directory
mkdir build && cd build

# 3. Configure with CMake
cmake .. -DCMAKE_BUILD_TYPE=Release

# 4. Build
make -j$(nproc)  # Linux
# or
make -j$(sysctl -n hw.ncpu)  # macOS
```

## Platform-Specific Notes

### macOS

- **Event System**: Automatically uses kqueue instead of epoll
- **Architecture**: Supports both Intel (x86_64) and Apple Silicon (ARM64)
- **Minimum Version**: macOS 10.15 (Catalina) or later

Key features on macOS:
- Native kqueue event notification
- Universal binary support (when applicable)
- Proper code signing compatibility

### Linux

- **Event System**: Uses epoll for high-performance I/O
- **Distributions**: Tested on Ubuntu, CentOS, Debian
- **Kernel**: Requires Linux 2.6+ for epoll support

## Build Outputs

After successful build, you'll find:

### Libraries
- `lib/libcoco.a` - Main Coco library
- `lib/libst.a` - State threads library
- `lib/libssl.a`, `lib/libcrypto.a` - OpenSSL libraries (built from source)

### Examples
- `bin/pingpong_server_tcp` - TCP echo server
- `bin/pingpong_client_tcp` - TCP echo client
- `bin/pingpong_server_udp` - UDP echo server
- `bin/pingpong_client_udp` - UDP echo client
- `bin/http_server` - HTTP/HTTPS server
- `bin/http_client` - HTTP/HTTPS client
- `bin/ws_client` - WebSocket client

## Verification

After building, verify the installation:

```bash
cd build

# Check that examples were built
ls -la bin/

# Test a simple server (in one terminal)
./bin/pingpong_server_tcp 8080

# Test client (in another terminal)
./bin/pingpong_client_tcp 127.0.0.1 8080
```

You should see the server using kqueue on macOS:
```
st_set_eventsys to kqueue
```

Or epoll on Linux:
```
st_set_eventsys to epoll
```

## Troubleshooting

### Common Issues

1. **Missing submodules**
   ```bash
   git submodule update --init --recursive
   ```

2. **CMake not found**
   - macOS: `brew install cmake`
   - Linux: `sudo apt-get install cmake`

3. **Build failures**
   - Check verbose output: `./build.sh -v`
   - Clean build: `./build.sh -c`
   - Check logs in `build/` directory

4. **OpenSSL issues**
   - The build script automatically builds OpenSSL from source
   - Ensure no conflicting OpenSSL installations

5. **Architecture issues**
   - The build system automatically detects native architecture
   - For universal binaries, modify CMake configuration

6. **xcrun architecture mismatch (macOS)**
   ```
   xcrun: error: unable to load libxcrun (mach-o file, but is an incompatible architecture)
   ```

   **Solution**: This occurs when running ARM64 shell with x86_64 Xcode tools. The build script now handles this automatically, but you can also:
   - Install ARM64 Xcode Command Line Tools: `xcode-select --install`
   - Or run the build under Rosetta 2: `arch -x86_64 ./build.sh`
   - Or use a terminal that's running under Rosetta 2

7. **"Error: No working C++ compiler found" on Apple Silicon**
   This typically occurs when running native ARM64 but having only x86_64 development tools.

   **Solution**: The build script now automatically detects this scenario and configures the build appropriately. You should see:
   ```
   Platform: macos (arm64 (x86_64 tools))
   Warning: ARM64 system with x86_64 development tools detected
   The build will be configured for x86_64 architecture
   ```

   If the script still fails, try:
   - Run explicitly under Rosetta 2: `arch -x86_64 ./build.sh`
   - Install ARM64 development tools: `xcode-select --install`
   - Install ARM64 compilers via Homebrew: `brew install gcc`

### Platform-Specific Issues

#### macOS
- **Xcode Command Line Tools**: Run `xcode-select --install`
- **Homebrew**: Ensure Homebrew is properly installed for dependencies
- **Code Signing**: No special configuration needed for development

#### Linux
- **Missing headers**: Install development packages
- **Old CMake**: Update to newer version (≥ 3.1 required)
- **Compiler issues**: Ensure GCC/G++ or Clang is installed

## Performance Optimization

### Build Performance
- Use parallel jobs: `./build.sh -j $(nproc)` or `./build.sh -j $(sysctl -n hw.ncpu)`
- Skip examples for faster builds: `./build.sh --no-examples`
- Use Release mode for production: `./build.sh` (default)

### Runtime Performance
- Release builds are optimized (`-O3`)
- Debug builds include symbols and assertions
- State threads provide efficient coroutine management

## Development Workflow

### For Contributors
1. Use debug builds during development: `./build.sh -d`
2. Enable verbose output for debugging: `./build.sh -v`
3. Clean build when changing CMake files: `./build.sh -c`

### For Production
1. Use release builds: `./build.sh`
2. Consider stripping debug symbols
3. Test on target platforms

## Next Steps

After building:
1. Explore the examples in `build/bin/`
2. Read the API documentation
3. Check the examples source code in `examples/`
4. Review the project analysis in `PROJECT_ANALYSIS.md`

## Support

For build issues:
1. Check this documentation first
2. Review the build script output and logs
3. Verify all prerequisites are installed
4. Check platform-specific notes above

The build system is designed to be robust and cross-platform while providing optimal performance on each supported platform.