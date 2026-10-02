#!/usr/bin/env bash
# Runs test-local.sh in Ubuntu 24.04 containers, one per architecture, side by side.
#
#   .harness/scripts/test-linux.sh [--keep] [amd64] [arm64]     default: both
#
# Containers are named coco-linux-<arch>. One that already runs is reused, together with
# its build trees, so a second run only rebuilds what changed; --keep leaves them running
# for that, otherwise they are removed at the end. No image is tagged: the base image is
# pulled by digest, so the local ubuntu:24.04 tag is never moved to another architecture.
# Extra options for test-local.sh go in TEST_ARGS, e.g. TEST_ARGS=--asan.
#
# Linux aarch64 is a known failure (testing.md); the log shows whether its reason changed.

set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# ubuntu:24.04, the multi-architecture index.
IMAGE=${IMAGE:-ubuntu@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517}
LOG_DIR=${LOG_DIR:-${TMPDIR:-/tmp}}
LOG_DIR=${LOG_DIR%/}
TEST_ARGS=${TEST_ARGS:-}

KEEP=0
ARCHS=()
for arg in "$@"; do
    case "$arg" in
        --keep) KEEP=1 ;;
        amd64 | arm64) ARCHS+=("$arg") ;;
        *) echo "usage: $0 [--keep] [amd64] [arm64]" >&2; exit 2 ;;
    esac
done
[ ${#ARCHS[@]} -eq 0 ] && ARCHS=(amd64 arm64)

SRC_TAR=$(mktemp "${TMPDIR:-/tmp}/coco-src.XXXXXX")
trap 'rm -f "$SRC_TAR"' EXIT
# No macOS resource forks (._*) and no build trees.
(cd "$ROOT" && COPYFILE_DISABLE=1 tar --no-xattrs -czf "$SRC_TAR" \
    --exclude=./build --exclude=./dist --exclude=./thirdparty/temp --exclude=./.git \
    --exclude='./build-*' .) || exit 1

run_arch() {
    local arch=$1 name=coco-linux-$1
    if [ -z "$(docker ps -q -f name="^$name$")" ]; then
        docker rm -f "$name" > /dev/null 2>&1
        docker run -d --name "$name" --platform "linux/$arch" "$IMAGE" sleep infinity > /dev/null ||
            { echo "$arch: container failed to start"; return 1; }
    fi
    docker exec "$name" bash -c 'command -v cmake > /dev/null || {
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -qq > /dev/null &&
            apt-get install -y -qq build-essential cmake perl pkg-config libssl-dev > /dev/null 2>&1; }' ||
        { echo "$arch: installing the toolchain failed"; return 1; }
    # Unpacked over the previous copy: unchanged files keep their times and are not rebuilt.
    docker exec "$name" mkdir -p /src /b &&
        docker cp "$SRC_TAR" "$name:/src.tgz" > /dev/null &&
        docker exec "$name" tar -xzf /src.tgz -C /src || { echo "$arch: copying the source failed"; return 1; }
    # QEMU's emulated x86_64 crashes the compiler less with fewer jobs.
    local jobs=""
    [ "$arch" = amd64 ] && [ "$(uname -m)" != x86_64 ] && jobs="JOBS=2"
    # shellcheck disable=SC2086
    docker exec "$name" env BUILD_ROOT=/b $jobs bash /src/.harness/scripts/test-local.sh $TEST_ARGS
}

pids=()
for arch in "${ARCHS[@]}"; do
    log="$LOG_DIR/coco-linux-$arch.log"
    echo "linux/$arch: running, log in $log"
    run_arch "$arch" > "$log" 2>&1 &
    pids+=($!)
done

status=0
for i in "${!ARCHS[@]}"; do
    arch=${ARCHS[$i]}
    wait "${pids[$i]}" || status=1
    echo
    echo "== linux/$arch"
    # The summary of test-local.sh, or the tail of the log when it never got that far.
    if grep -q "^== summary" "$LOG_DIR/coco-linux-$arch.log"; then
        sed -n '/^== summary/,$p' "$LOG_DIR/coco-linux-$arch.log" | tail -n +2
    else
        tail -n 15 "$LOG_DIR/coco-linux-$arch.log"
    fi
    if [ $KEEP -eq 0 ]; then
        docker rm -f "coco-linux-$arch" > /dev/null 2>&1
    fi
done
exit $status
