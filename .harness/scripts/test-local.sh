#!/usr/bin/env bash
# Runs the checks of .harness/testing.md on this machine: the four link combinations (ctest,
# then every case in one process), the install check, and on request ASan and TSan.
#
#   .harness/scripts/test-local.sh [--asan] [--tsan]
#
# BUILD_ROOT  where the build trees go, default build/matrix in the repository
# JOBS        parallel build jobs, default the number of CPUs
# RETRIES     builds retried after a compiler segfault (QEMU), default 30
#
# Exits non-zero when anything failed; the summary at the end lists every result.

set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SCRIPTS="$ROOT/.harness/scripts"
BUILD_ROOT=${BUILD_ROOT:-$ROOT/build/matrix}
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}
RETRIES=${RETRIES:-30}

ASAN=0
TSAN=0
for arg in "$@"; do
    case "$arg" in
        --asan) ASAN=1 ;;
        --tsan) TSAN=1 ;;
        *) echo "usage: $0 [--asan] [--tsan]" >&2; exit 2 ;;
    esac
done

SYSTEM_SSL=(-DCOCO_USE_SYSTEM_OPENSSL=ON)
if [ "$(uname)" = Darwin ]; then
    # Homebrew's OpenSSL is not on the default search path.
    SYSTEM_SSL+=(-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)")
fi

SUMMARY=()
FAILED=0
report() {
    SUMMARY+=("$1")
    echo "$1"
}
fail() {
    FAILED=1
    report "$1"
}

# Configures and builds $1 with the remaining cmake arguments. QEMU makes the compiler
# segfault at random; cmake --build resumes, so such a build is simply run again.
build() {
    local dir=$1 target=$2
    shift 2
    mkdir -p "$dir"
    if ! cmake -S "$ROOT" -B "$dir" "$@" > "$dir.configure.log" 2>&1; then
        fail "$(basename "$dir"): configure FAILED, see $dir.configure.log"
        return 1
    fi
    local i
    for i in $(seq 1 "$RETRIES"); do
        if cmake --build "$dir" -j "$JOBS" $target > "$dir.build.log" 2>&1; then
            return 0
        fi
        if ! grep -q "Segmentation fault" "$dir.build.log"; then
            break
        fi
        echo "$(basename "$dir"): compiler segfault, retry $i"
    done
    grep -E "error" "$dir.build.log" | head -10
    fail "$(basename "$dir"): build FAILED: $(grep -m1 "error" "$dir.build.log" | sed 's/^ *//'); see $dir.build.log"
    return 1
}

# ctest, then all cases in one process, which finds state a case leaves behind.
run_tests() {
    local dir=$1 name
    name=$(basename "$dir")
    (cd "$dir" && ctest -j1 --output-on-failure > ctest.log 2>&1)
    local ctest_ok=$? passed
    passed=$(grep "tests passed" "$dir/ctest.log")
    (cd "$dir" && ./bin/coco_tests > single.log 2>&1)
    local single_ok=$?
    if [ $ctest_ok -eq 0 ] && [ $single_ok -eq 0 ]; then
        report "$name: ctest $passed; single process ok"
    else
        fail "$name: ctest $passed (exit $ctest_ok); single process exit $single_ok; see $dir/ctest.log, $dir/single.log"
        grep -E "\*\*\*" "$dir/ctest.log" | head -10
    fi
}

# Installs $1 to a temporary prefix and runs the consumer built both ways.
install_check() {
    local dir=$1 kind=$2
    local prefix="$BUILD_ROOT/prefix-$kind" out="$BUILD_ROOT/consumer-$kind"
    rm -rf "$prefix" "$out"
    if ! cmake --install "$dir" --prefix "$prefix" > "$out.install.log" 2>&1; then
        fail "install $kind: FAILED, see $out.install.log"
        return
    fi
    if cmake -S "$SCRIPTS/consumer" -B "$out" -DCMAKE_PREFIX_PATH="$prefix" > "$out.log" 2>&1 &&
        retry_segfault "$out.log" cmake --build "$out" && "$out/consumer" >> "$out.log" 2>&1; then
        report "install $kind: find_package ok"
    else
        fail "install $kind: find_package FAILED (exit $?), see $out.log"
        tail -n 20 "$out.log"
    fi
    local static=""
    [ "$kind" = static ] && static=--static
    local flags
    flags=$(PKG_CONFIG_PATH="$prefix/lib/pkgconfig" pkg-config $static --cflags --libs coco)
    # shellcheck disable=SC2086
    if retry_segfault "$out.log" c++ -std=c++11 "$SCRIPTS/consumer/main.cpp" $flags \
            -Wl,-rpath,"$prefix/lib" -o "$out-pc" && "$out-pc" >> "$out.log" 2>&1; then
        report "install $kind: pkg-config ok"
    else
        fail "install $kind: pkg-config FAILED (exit $?), see $out.log"
        tail -n 20 "$out.log"
    fi
}

# Runs the command, appending its output to $1, again as long as it dies of a segfault.
retry_segfault() {
    local log=$1 i
    shift
    for i in $(seq 1 "$RETRIES"); do
        local mark
        mark=$(wc -l < "$log")
        "$@" >> "$log" 2>&1 && return 0
        tail -n +"$((mark + 1))" "$log" | grep -q "Segmentation fault" || return 1
    done
    return 1
}

echo "coco $(uname -s) $(uname -m), build trees in $BUILD_ROOT"
mkdir -p "$BUILD_ROOT"

matrix() {
    local name=$1
    shift
    build "$BUILD_ROOT/$name" "" -DCMAKE_BUILD_TYPE=Release "$@" && run_tests "$BUILD_ROOT/$name"
}
matrix static
matrix static-sys "${SYSTEM_SSL[@]}"
matrix shared -DBUILD_SHARED_LIBS=ON
matrix shared-sys -DBUILD_SHARED_LIBS=ON "${SYSTEM_SSL[@]}"
[ -x "$BUILD_ROOT/static/bin/coco_tests" ] && install_check "$BUILD_ROOT/static" static
[ -x "$BUILD_ROOT/shared/bin/coco_tests" ] && install_check "$BUILD_ROOT/shared" shared

if [ $ASAN -eq 1 ]; then
    # -O0 frames are larger than a 64KB coroutine stack, so not Debug.
    dir="$BUILD_ROOT/asan"
    if build "$dir" "--target coco_tests" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCOCO_ENABLE_ASAN=ON -DCOCO_BUILD_EXAMPLES=OFF; then
        if (cd "$dir" && ASAN_OPTIONS=detect_stack_use_after_return=0 \
                ctest -j1 --output-on-failure > ctest.log 2>&1); then
            report "asan: ctest $(grep 'tests passed' "$dir/ctest.log")"
        else
            fail "asan: ctest $(grep 'tests passed' "$dir/ctest.log"), see $dir/ctest.log"
        fi
    fi
fi

if [ $TSAN -eq 1 ]; then
    dir="$BUILD_ROOT/tsan"
    tsan=-fsanitize=thread
    if build "$dir" "--target coco_tests" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCOCO_BUILD_EXAMPLES=OFF -DCMAKE_C_FLAGS=$tsan -DCMAKE_CXX_FLAGS=$tsan \
            -DCMAKE_EXE_LINKER_FLAGS=$tsan -DCMAKE_SHARED_LINKER_FLAGS=$tsan; then
        # Never passes under TSan, see testing.md.
        (cd "$dir" && ctest -j1 --output-on-failure \
            -E '^RuntimeSecondSignalEndsStuckProcess$' > ctest.log 2>&1)
        ok=$?
        races=$(grep -c "WARNING: ThreadSanitizer" "$dir/ctest.log")
        if [ $ok -eq 0 ] && [ "$races" -eq 0 ]; then
            report "tsan: ctest $(grep 'tests passed' "$dir/ctest.log"); no warnings"
        else
            fail "tsan: ctest exit $ok, $races warnings, see $dir/ctest.log"
        fi
    fi
fi

echo
echo "== summary ($(uname -s) $(uname -m))"
printf '%s\n' "${SUMMARY[@]}"
exit $FAILED
