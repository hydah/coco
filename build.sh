#!/bin/bash

# Coco Networking Library Build Script
# Supports Linux, macOS (Intel & Apple Silicon)
#
# Usage: ./build.sh [options]
# Options:
#   -h, --help          Show this help message
#   -c, --clean         Clean build directory before building
#   -d, --debug         Build in debug mode (default: Release)
#   -j, --jobs N        Number of parallel jobs (default: auto-detect)
#   -t, --test          Run tests after building
#   -i, --install       Install after building
#   -v, --verbose       Verbose output
#   --no-examples       Don't build examples

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values
BUILD_TYPE="Release"
CLEAN_BUILD=false
JOBS=""
RUN_TESTS=false
INSTALL=false
VERBOSE=false
BUILD_EXAMPLES=true
CMAKE_CMD="cmake"

# Platform detection
detect_platform() {
    case "$(uname -s)" in
        Linux*)
            PLATFORM="linux"
            ;;
        Darwin*)
            PLATFORM="macos"
            ;;
        *)
            echo -e "${RED}Error: Unsupported platform: $(uname -s)${NC}"
            exit 1
            ;;
    esac

    # Architecture detection with comprehensive Rosetta 2 and tool compatibility handling
    case "$(uname -m)" in
        x86_64)
            # Check if we're running under Rosetta 2
            if [ "$PLATFORM" = "macos" ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" = "1" ]; then
                ARCH="arm64 (running under Rosetta 2)"
                NATIVE_ARCH="arm64"
                RUNNING_UNDER_ROSETTA=true
            else
                ARCH="x86_64"
                NATIVE_ARCH="x86_64"
                RUNNING_UNDER_ROSETTA=false
            fi
            ;;
        arm64|aarch64)
            ARCH="arm64"
            NATIVE_ARCH="arm64"
            RUNNING_UNDER_ROSETTA=false
            ;;
        *)
            ARCH="$(uname -m)"
            NATIVE_ARCH="$ARCH"
            RUNNING_UNDER_ROSETTA=false
            ;;
    esac

    echo -e "${BLUE}Platform: $PLATFORM ($ARCH)${NC}"

    # Check for development tools architecture compatibility on macOS
    if [ "$PLATFORM" = "macos" ]; then
        check_development_tools_architecture
    fi

    # Warn about architecture mismatch
    if [ "$PLATFORM" = "macos" ] && [ "$RUNNING_UNDER_ROSETTA" = true ]; then
        echo -e "${YELLOW}Note: Running under Rosetta 2 translation layer${NC}"
        echo -e "${YELLOW}Native architecture: $NATIVE_ARCH, Current architecture: x86_64${NC}"
    fi
}

# Help function
show_help() {
    cat << EOF
Coco Networking Library Build Script

Usage: $0 [OPTIONS]

OPTIONS:
    -h, --help          Show this help message
    -c, --clean         Clean build directory before building
    -d, --debug         Build in debug mode (default: Release)
    -j, --jobs N        Number of parallel jobs (default: auto-detect)
    -t, --test          Run tests after building
    -i, --install       Install after building
    -v, --verbose       Verbose output
    --no-examples       Don't build examples

EXAMPLES:
    $0                  # Build with default settings
    $0 -c -d            # Clean build in debug mode
    $0 -j 8 -v          # Build with 8 jobs, verbose output
    $0 -c -t -i         # Clean, build, test, and install

EOF
}

# Parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                show_help
                exit 0
                ;;
            -c|--clean)
                CLEAN_BUILD=true
                shift
                ;;
            -d|--debug)
                BUILD_TYPE="Debug"
                shift
                ;;
            -j|--jobs)
                JOBS="$2"
                shift 2
                ;;
            -t|--test)
                RUN_TESTS=true
                shift
                ;;
            -i|--install)
                INSTALL=true
                shift
                ;;
            -v|--verbose)
                VERBOSE=true
                shift
                ;;
            --no-examples)
                BUILD_EXAMPLES=false
                shift
                ;;
            *)
                echo -e "${RED}Error: Unknown option $1${NC}"
                show_help
                exit 1
                ;;
        esac
    done
}

# Check dependencies
check_dependencies() {
    echo -e "${BLUE}Checking dependencies...${NC}"

    # Check for CMake
    if ! command -v cmake &> /dev/null; then
        echo -e "${RED}Error: CMake is not installed${NC}"
        if [ "$PLATFORM" = "macos" ]; then
            echo -e "${YELLOW}Install with: brew install cmake${NC}"
        else
            echo -e "${YELLOW}Install with: sudo apt-get install cmake (Ubuntu/Debian) or equivalent${NC}"
        fi
        exit 1
    fi

    # Check for make
    if ! command -v make &> /dev/null; then
        echo -e "${RED}Error: make is not installed${NC}"
        exit 1
    fi

    # Check for C++ compiler with architecture compatibility
    local compiler_found=false
    local compiler_arch=""

    for compiler in c++ g++ clang++; do
        if command -v $compiler > /dev/null 2>&1; then
            # Check if compiler works and get its architecture
            if $compiler --version > /dev/null 2>&1; then
                compiler_found=true
                # Try to compile a simple test to check architecture compatibility
                echo "int main() { return 0; }" > /tmp/test_arch.cc
                if $compiler /tmp/test_arch.cc -o /tmp/test_arch > /dev/null 2>&1; then
                    compiler_arch=$(file /tmp/test_arch | grep -o 'x86_64\|arm64\|aarch64' | head -1)
                    rm -f /tmp/test_arch.cc /tmp/test_arch
                    break
                fi
                rm -f /tmp/test_arch.cc /tmp/test_arch
            fi
        fi
    done

    if [ "$compiler_found" = false ]; then
        echo -e "${RED}Error: No working C++ compiler found${NC}"
        exit 1
    fi

    # Check for architecture compatibility issues
    if [ "$PLATFORM" = "macos" ] && [ "$RUNNING_UNDER_ROSETTA" = true ]; then
        echo -e "${YELLOW}Warning: Running x86_64 compiler under Rosetta 2 on ARM64 system${NC}"
        echo -e "${YELLOW}This may cause compatibility issues. Consider installing native ARM64 tools.${NC}"
    fi

    # Check for git (for submodules)
    if ! command -v git &> /dev/null; then
        echo -e "${RED}Error: git is not installed${NC}"
        exit 1
    fi

    echo -e "${GREEN}All dependencies found${NC}"
}

# Check development tools architecture compatibility on macOS
check_development_tools_architecture() {
    echo -e "${BLUE}Checking development tools architecture compatibility...${NC}"

    local has_working_compiler=false
    local compiler_arch=""
    local native_arch="$(uname -m)"

    # Test each available compiler
    for compiler in c++ g++ clang++; do
        if command -v $compiler > /dev/null 2>&1; then
            echo -e "${BLUE}Testing $compiler...${NC}"

            # Create a simple test program
            echo "int main() { return 0; }" > /tmp/test_arch_$$.cc

            if $compiler /tmp/test_arch_$$.cc -o /tmp/test_arch_$$ > /dev/null 2>&1; then
                # Get the architecture of the compiled binary
                compiler_arch=$(file /tmp/test_arch_$$ | grep -o 'x86_64\|arm64\|aarch64' | head -1)

                if [ -n "$compiler_arch" ]; then
                    echo -e "${GREEN}$compiler: Working ($compiler_arch architecture)${NC}"
                    has_working_compiler=true

                    # Check for architecture mismatch
                    if [ "$native_arch" = "arm64" ] && [ "$compiler_arch" = "x86_64" ]; then
                        echo -e "${YELLOW}Warning: ARM64 system with x86_64 development tools detected${NC}"
                        echo -e "${YELLOW}This is common on Apple Silicon Macs with x86_64-only Xcode tools${NC}"
                        echo -e "${YELLOW}The build will be configured for x86_64 architecture${NC}"

                        # Set flags for x86_64 build
                        RUNNING_UNDER_ROSETTA=true
                        ARCH="arm64 (x86_64 tools)"
                        NATIVE_ARCH="arm64"
                    fi
                else
                    echo -e "${YELLOW}$compiler: Working (unknown architecture)${NC}"
                    has_working_compiler=true
                fi

                rm -f /tmp/test_arch_$$.cc /tmp/test_arch_$$
                break
            else
                echo -e "${RED}$compiler: Not working${NC}"
                rm -f /tmp/test_arch_$$.cc /tmp/test_arch_$$
            fi
        else
            echo -e "${YELLOW}$compiler: Not found${NC}"
        fi
    done

    if [ "$has_working_compiler" = false ]; then
        echo -e "${RED}Error: No working C++ compiler found${NC}"

        # Provide architecture-specific guidance
        if [ "$native_arch" = "arm64" ]; then
            echo -e "${YELLOW}On Apple Silicon Macs, you may need to:${NC}"
            echo -e "${YELLOW}1. Install Xcode Command Line Tools: xcode-select --install${NC}"
            echo -e "${YELLOW}2. Run the build under Rosetta 2: arch -x86_64 $0${NC}"
            echo -e "${YELLOW}3. Install ARM64 development tools via Homebrew${NC}"
        else
            echo -e "${YELLOW}Install a C++ compiler (g++, clang++, or c++)${NC}"
        fi

        exit 1
    fi
}

# Initialize git submodules
init_submodules() {
    echo -e "${BLUE}Initializing git submodules...${NC}"
    if [ -d ".git" ]; then
        git submodule update --init --recursive
        echo -e "${GREEN}Submodules initialized${NC}"
    else
        echo -e "${YELLOW}Not a git repository, skipping submodule initialization${NC}"
    fi
}

# Setup build environment
setup_build_env() {
    # Auto-detect number of jobs if not specified
    if [ -z "$JOBS" ]; then
        if command -v nproc &> /dev/null; then
            JOBS=$(nproc)
        elif command -v sysctl &> /dev/null; then
            JOBS=$(sysctl -n hw.ncpu)
        else
            JOBS=4
        fi
    fi

    echo -e "${BLUE}Using $JOBS parallel jobs${NC}"

    # Set verbose flag
    if [ "$VERBOSE" = true ]; then
        MAKE_FLAGS="VERBOSE=1"
    else
        MAKE_FLAGS=""
    fi
}

# Configure build
configure_build() {
    echo -e "${BLUE}Configuring build...${NC}"

    # Create build directory
    if [ "$CLEAN_BUILD" = true ]; then
        echo -e "${YELLOW}Cleaning build directory...${NC}"
        rm -rf build
    fi

    mkdir -p build
    cd build

    # Configure CMake
    CMAKE_ARGS="-DCMAKE_BUILD_TYPE=$BUILD_TYPE"

    if [ "$BUILD_EXAMPLES" = false ]; then
        CMAKE_ARGS="$CMAKE_ARGS -DCOCO_BUILD_EXAMPLES=OFF"
    fi

    # Platform-specific configurations
    case "$PLATFORM" in
        macos)
            echo -e "${BLUE}Configuring for macOS...${NC}"

            # Handle architecture mismatch issues
            if [ "$RUNNING_UNDER_ROSETTA" = true ]; then
                echo -e "${YELLOW}Configuring for x86_64 build under Rosetta 2${NC}"
                # Force x86_64 architecture when running under Rosetta
                CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_OSX_ARCHITECTURES=x86_64"
            fi

            # Try to find a working SDK
            if [ -d "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk" ]; then
                CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk"
            elif [ -d "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk" ]; then
                CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_OSX_SYSROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
            fi
            ;;
        linux)
            echo -e "${BLUE}Configuring for Linux...${NC}"
            ;;
    esac

    echo -e "${BLUE}Running: cmake $CMAKE_ARGS ..${NC}"
    if [ "$VERBOSE" = true ]; then
        cmake $CMAKE_ARGS ..
    else
        cmake $CMAKE_ARGS .. > cmake_output.log 2>&1
        if [ $? -ne 0 ]; then
            echo -e "${RED}CMake configuration failed. Check cmake_output.log${NC}"
            exit 1
        fi
    fi

    echo -e "${GREEN}Configuration complete${NC}"
}

# Build the project
build_project() {
    echo -e "${BLUE}Building project ($BUILD_TYPE mode)...${NC}"

    if [ "$VERBOSE" = true ]; then
        make -j$JOBS $MAKE_FLAGS
    else
        make -j$JOBS > build_output.log 2>&1
        if [ $? -ne 0 ]; then
            echo -e "${RED}Build failed. Check build_output.log${NC}"
            exit 1
        fi
    fi

    echo -e "${GREEN}Build complete${NC}"
}

# Run tests
run_tests() {
    if [ "$RUN_TESTS" = true ]; then
        echo -e "${BLUE}Running tests...${NC}"
        # Add test execution here when tests are available
        echo -e "${YELLOW}No tests configured yet${NC}"
    fi
}

# Install the project
install_project() {
    if [ "$INSTALL" = true ]; then
        echo -e "${BLUE}Installing project...${NC}"
        if [ "$VERBOSE" = true ]; then
            make install
        else
            make install > install_output.log 2>&1
            if [ $? -ne 0 ]; then
                echo -e "${RED}Installation failed. Check install_output.log${NC}"
                exit 1
            fi
        fi
        echo -e "${GREEN}Installation complete${NC}"
    fi
}

# Show build summary
show_summary() {
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}Build Summary${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo -e "${BLUE}Platform:${NC} $PLATFORM ($ARCH)"
    echo -e "${BLUE}Build Type:${NC} $BUILD_TYPE"
    echo -e "${BLUE}Parallel Jobs:${NC} $JOBS"
    echo -e "${BLUE}Build Examples:${NC} $BUILD_EXAMPLES"
    echo -e "${BLUE}Run Tests:${NC} $RUN_TESTS"
    echo -e "${BLUE}Install:${NC} $INSTALL"
    echo -e "${GREEN}========================================${NC}"

    # Show available binaries
    if [ -d "bin" ]; then
        echo -e "${BLUE}Available binaries:${NC}"
        ls -1 bin/ 2>/dev/null | sed 's/^/  /'
    fi

    # Show available libraries
    if [ -d "lib" ]; then
        echo -e "${BLUE}Available libraries:${NC}"
        ls -1 lib/*.a 2>/dev/null | sed 's/^/  /'
    fi

    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}Build completed successfully!${NC}"
    echo -e "${YELLOW}To run examples:${NC}"
    echo -e "  cd build"
    echo -e "  ./bin/pingpong_server_tcp 8080"
    echo -e "  ./bin/http_server 8080"
    echo -e "${GREEN}========================================${NC}"
}

# Main function
main() {
    echo -e "${BLUE}Coco Networking Library Build Script${NC}"
    echo -e "${BLUE}====================================${NC}"

    # Parse arguments
    parse_args "$@"

    # Detect platform
    detect_platform

    # Check dependencies
    check_dependencies

    # Initialize submodules
    init_submodules

    # Setup build environment
    setup_build_env

    # Configure build
    configure_build

    # Build project
    build_project

    # Run tests
    run_tests

    # Install
    install_project

    # Show summary
    show_summary
}

# Run main function
main "$@"