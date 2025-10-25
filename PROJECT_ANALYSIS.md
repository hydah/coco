# Coco Networking Library - Project Analysis

> **Document Purpose**: Comprehensive analysis of the Coco networking library architecture, functionality, and design principles for development reference.
>
> **Last Updated**: 2025-10-25
>
> **Status**: Active Development Reference

## Table of Contents

1. [Project Overview](#project-overview)
2. [Core Functionality](#core-functionality)
3. [Architecture Analysis](#architecture-analysis)
4. [Design Principles](#design-principles)
5. [Key Components](#key-components)
6. [Performance Characteristics](#performance-characteristics)
7. [Development Guidelines](#development-guidelines)
8. [Common Patterns](#common-patterns)
9. [Error Handling](#error-handling)
10. [Testing Considerations](#testing-considerations)

---

## Project Overview

**Coco** is a high-performance, coroutine-based C++ networking library designed for building scalable network applications. It provides an elegant solution to the C10K problem by implementing cooperative multitasking through integration with the State Threads library.

### Key Facts
- **Language**: C++11
- **Build System**: CMake
- **Dependencies**: State Threads, OpenSSL, Node.js http-parser
- **Architecture**: Coroutine-based event-driven networking
- **Primary Use Case**: High-concurrency network servers and clients

---

## Core Functionality

### 1. Coroutine System
- **M:N Threading Model**: Multiple coroutines per OS thread
- **Stackful Coroutines**: Each coroutine has dedicated stack (64KB default)
- **Cooperative Multitasking**: Voluntary yielding during I/O operations
- **Automatic Context Switching**: Transparent to developer

### 2. Network Protocol Support
```
Transport Layer:
├── TCP (StreamConn)
├── UDP (DatagramConn)
└── SSL/TLS (SslConn)

Application Layer:
├── HTTP/1.1 (HttpServer, HttpClient)
└── WebSocket (WebSocketConn)
```

### 3. I/O Operations
- **Non-blocking by Default**: All I/O operations yield when blocking
- **Timeout Support**: Configurable timeouts for all operations
- **Vectorized I/O**: `writev`/`readv` support for efficiency
- **Connection Pooling**: Reusable connection resources

---

## Architecture Analysis

### Layered Architecture
```
┌─────────────────────────────────────┐
│  Application Protocols (HTTP/WS)   │
├─────────────────────────────────────┤
│    Protocol Layer (Parsing)        │
├─────────────────────────────────────┤
│  Transport Layer (TCP/UDP/SSL)     │
├─────────────────────────────────────┤
│     Socket Layer (Raw I/O)         │
├─────────────────────────────────────┤
│   State Threads (Coroutine Mgmt)   │
└─────────────────────────────────────┘
```

### Key Architectural Decisions

#### 1. State Thread Integration
- **Event System**: Uses epoll (Linux) or kqueue (BSD) for scalable I/O
- **Coroutine Scheduling**: M:N mapping of coroutines to OS threads
- **Memory Efficiency**: ~64KB per coroutine vs MBs per thread

#### 2. I/O Abstraction
```cpp
// Base I/O interfaces
class IoReader {
    virtual int Read(void* buf, size_t size, ssize_t* nread) = 0;
};

class IoWriter {
    virtual int Write(void* buf, size_t size, ssize_t* nwrite) = 0;
};
```

#### 3. Connection Management
- **Lifecycle Tracking**: Clear states from accept to cleanup
- **Zombie Collection**: Deferred cleanup for safe deletion
- **Resource Isolation**: Each connection in separate coroutine

---

## Design Principles

### 1. Simplicity First
> **Principle**: Write synchronous-looking code that actually performs asynchronous operations

**Implementation**:
```cpp
// This code yields automatically when data not available
int ret = conn->Read(buf, sizeof(buf), &nread);
if (ret != 0) break;
```

### 2. Performance Without Sacrifice
> **Principle**: High-level abstractions should not compromise low-level performance

**Strategies**:
- Zero-copy operations where possible
- Buffer reuse and pooling
- Vectorized I/O operations
- Lock-free design with thread-local storage

### 3. Resource Safety
> **Principle**: Resources should be automatically managed and protected

**Mechanisms**:
- RAII patterns throughout codebase
- Smart pointers with custom deleters
- Automatic cleanup on coroutine termination
- Memory bounds checking and limits

### 4. Comprehensive Error Handling
> **Principle**: Errors should be caught early, reported clearly, and handled gracefully

**Error Categories**:
- **1000-1099**: Socket and network errors
- **1050-1099**: System operation errors
- **1060-1069**: SSL/TLS specific errors
- **1070-1079**: Thread and coroutine errors
- **3000-3099**: HTTP request errors
- **4000-4099**: HTTP response errors

---

## Key Components

### 1. Coroutine Infrastructure
```
src/base/
├── coroutine.hpp/cpp          # Base coroutine class
├── coroutine_mgr.hpp/cpp      # Coroutine management
└── coroutine_context.hpp/cpp  # Thread-local context
```

**Key Classes**:
- `CoCoroutine`: Base coroutine with lifecycle management
- `CoroutineHandler`: Abstract interface for coroutine logic
- `CoroutineContext`: Thread-local ID and context management

### 2. Network Layer
```
src/net/
├── layer4/                    # Transport layer
│   ├── layer4_conn.hpp/cpp   # Base transport abstraction
│   ├── stream_conn.hpp/cpp   # TCP/SSL connections
│   └── datagram_conn.hpp/cpp # UDP connections
└── layer7/                    # Application layer
    ├── http/                  # HTTP implementation
    └── websocket/             # WebSocket implementation
```

### 3. Protocol Implementations

#### HTTP Protocol
- **Full HTTP/1.1 Support**: Complete request/response handling
- **Chunked Encoding**: Streaming response support
- **Keep-Alive**: Connection reuse optimization
- **Message Parsing**: Uses Node.js http-parser library

#### WebSocket Protocol
- **RFC 6455 Compliance**: Complete implementation
- **Frame-Based API**: Clean message-oriented interface
- **Fragmentation Support**: Proper message reassembly
- **Security Features**: Frame size limits (4MB max), proper masking

---

## Performance Characteristics

### Memory Usage
- **Per Coroutine**: ~64KB stack + application data
- **Per Connection**: Base connection overhead + protocol-specific data
- **Buffer Management**: Reusable buffers with smart pointer management

### Scalability Metrics
- **Concurrency**: 10,000+ simultaneous connections
- **Context Switch**: Microseconds (vs milliseconds for threads)
- **Memory Efficiency**: Significant savings vs thread-per-connection model

### Optimization Strategies
1. **Buffer Reuse**: FastBuffer for protocol parsing
2. **Vectorized I/O**: `writev`/`readv` for scatter-gather operations
3. **Connection Pooling**: Reuse of connection objects
4. **Lazy Initialization**: Objects created only when needed

---

## Development Guidelines

### 1. Code Organization
- **Follow Layered Architecture**: Respect separation between layers
- **Use Existing Patterns**: Study similar implementations before coding
- **Maintain Consistency**: Follow established naming and structure conventions

### 2. Error Handling Requirements
```cpp
// Always check return codes
int ret = operation();
if (ret != 0) {
    // Provide context in error messages
    return ErrorCode(ret, "context about the operation");
}
```

### 3. Memory Management
- **Use RAII**: Prefer smart pointers and automatic cleanup
- **Set Limits**: Always implement bounds checking and size limits
- **Pool Resources**: Reuse objects where possible

### 4. Coroutine Safety
- **No Shared State**: Each coroutine should be independent
- **Proper Yielding**: Ensure I/O operations can yield when needed
- **Clean Shutdown**: Implement proper interrupt and cleanup logic

---

## Common Patterns

### 1. Server Pattern
```cpp
class ServerRoutine : public CoCoroutine {
    // Listen for connections
    // Accept new connections
    // Spawn connection handlers
    // Manage connection lifecycle
};
```

### 2. Connection Handler Pattern
```cpp
class ConnRoutine : public CoCoroutine {
    // Handle individual connection
    // Read/write operations
    // Protocol-specific processing
    // Cleanup on disconnect
};
```

### 3. Protocol Parser Pattern
```cpp
class ProtocolParser {
    // State machine for parsing
    // Buffer management
    // Error handling
    // Message assembly
};
```

---

## Error Handling

### Error Code System
```cpp
// Systematic error mapping
#define ERROR_SOCKET_CREATE    1000
#define ERROR_SOCKET_TIMEOUT   1011
#define ERROR_HTTP_PARSE_URI   3007
#define ERROR_SSL_HANDSHAKE    1060
```

### Error Propagation
- **Preserve Context**: Include operation context in error messages
- **Consistent Mapping**: Map system errors to application error codes
- **Graceful Handling**: Continue operation where possible

### Common Error Scenarios
1. **Network Errors**: Connection refused, timeout, reset
2. **Protocol Errors**: Malformed messages, invalid state
3. **Resource Errors**: Memory allocation failure, file descriptor exhaustion
4. **Timeout Errors**: Operation exceeded time limit

---

## Testing Considerations

### 1. Unit Testing
- **Mock I/O Operations**: Test logic without actual network I/O
- **Error Injection**: Test error handling paths
- **State Machine Testing**: Verify protocol state transitions

### 2. Integration Testing
- **Multi-connection Scenarios**: Test with many simultaneous connections
- **Protocol Compliance**: Verify RFC compliance for protocols
- **Performance Testing**: Measure under load

### 3. Stress Testing
- **Connection Limits**: Test maximum connection capacity
- **Memory Pressure**: Test under memory constraints
- **Error Recovery**: Test system behavior under error conditions

---

## Future Development Notes

### Areas for Enhancement
1. **Additional Protocols**: HTTP/2, gRPC, custom protocols
2. **Monitoring**: Enhanced metrics and observability
3. **Security**: Additional security features and hardening
4. **Performance**: Further optimization opportunities

### Maintenance Guidelines
1. **Preserve Architecture**: Maintain layered design
2. **Backward Compatibility**: Keep APIs stable
3. **Performance Regression**: Monitor performance changes
4. **Documentation**: Keep documentation current

---

## References

- **State Threads Library**: http://state-threads.sourceforge.net/
- **HTTP/1.1 RFC**: https://tools.ietf.org/html/rfc7230
- **WebSocket RFC**: https://tools.ietf.org/html/rfc6455
- **Project Examples**: See `examples/` directory for usage patterns

---

## macOS/ARM Build Instructions

### Platform Support
The project now supports macOS on both Intel (x86_64) and Apple Silicon (ARM64) architectures.

### Key Changes for macOS Compatibility

1. **Event System**: Automatically uses kqueue instead of epoll on macOS
2. **Assembly Code**: Fixed GNU assembler directives for macOS compatibility
3. **Platform Detection**: Added proper macOS platform detection in CMake
4. **OpenSSL**: Updated configuration for macOS builds
5. **State Threads**: Added ARM64 support to state-threads library
6. **Architecture Handling**: Sophisticated detection of Rosetta 2 and mixed environments

### Enhanced Build Script Features

The build script (`build.sh`) now includes:

- **Automatic Architecture Detection**: Detects native ARM64 vs x86_64 under Rosetta 2
- **Compiler Architecture Validation**: Checks for architecture mismatches and warns appropriately
- **SDK Path Resolution**: Automatically finds and configures the correct macOS SDK
- **Rosetta 2 Handling**: Properly configures builds when running under translation layer
- **Architecture-Specific Configuration**: Forces correct architecture flags for mixed environments

### Build Instructions

```bash
# Clone the repository
git clone <repository-url>
cd coco

# Initialize submodules
git submodule update --init --recursive

# Build with the enhanced script
chmod +x build.sh
./build.sh

# Or with verbose output to see architecture detection
./build.sh -v
```

### Architecture Detection Examples

The build script will show architecture information:

**Native ARM64 (Apple Silicon)**:
```
Platform: macos (arm64)
```

**x86_64 under Rosetta 2**:
```
Platform: macos (arm64 (running under Rosetta 2))
Note: Running under Rosetta 2 translation layer
Native architecture: arm64, Current architecture: x86_64
Warning: Running x86_64 compiler under Rosetta 2 on ARM64 system
```

### Verification

After building, you should see:
- `st_set_eventsys to kqueue` in the logs (indicating macOS event system is used)
- Successful compilation of all examples
- Working network servers on macOS
- Architecture-appropriate binaries in `build/bin/`

### Architecture Troubleshooting

The build script now provides comprehensive architecture handling for all Apple Silicon scenarios:

**Automatic Detection and Resolution:**
1. **Native ARM64 with x86_64 tools** (Most common): Automatically detects and configures x86_64 build
2. **Running under Rosetta 2**: Properly detects and handles translation layer
3. **Mixed architecture environments**: Provides clear warnings and appropriate configuration
4. **Compiler architecture validation**: Tests each compiler and determines its architecture

**Common Issues and Solutions:**

1. **"Error: No working C++ compiler found" on Apple Silicon**
   - **Cause**: Running native ARM64 but having only x86_64 development tools
   - **Solution**: The script now automatically detects this and configures for x86_64
   - **Output**: `Platform: macos (arm64 (x86_64 tools))`

2. **xcrun architecture mismatch**
   - **Cause**: ARM64 shell with x86_64 Xcode tools
   - **Solution**: Automatically handled with proper SDK configuration

3. **Architecture-specific build failures**
   - **Solution**: Multiple fallback mechanisms and clear error messages

**Manual Control Options:**
```bash
# Force x86_64 build on ARM64 system
arch -x86_64 ./build.sh

# Install native ARM64 Xcode tools
xcode-select --install

# Install ARM64 compilers via Homebrew
brew install gcc
```

**Architecture Detection Output Examples:**

**Native ARM64 with x86_64 tools**:
```
Platform: macos (arm64 (x86_64 tools))
Warning: ARM64 system with x86_64 development tools detected
The build will be configured for x86_64 architecture
```

**Running under Rosetta 2**:
```
Platform: macos (arm64 (running under Rosetta 2))
Note: Running under Rosetta 2 translation layer
Native architecture: arm64, Current architecture: x86_64
```

**Native ARM64 with ARM64 tools**:
```
Platform: macos (arm64)
```

The build system is designed to work seamlessly across all Apple Silicon configurations, providing optimal compatibility and clear guidance for resolution of any architecture-related issues.

---

*This document should be updated as the project evolves and new patterns or requirements emerge.*