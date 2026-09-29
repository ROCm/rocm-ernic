#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# NVMe-oF backend testing for CI.
#
# The controller only becomes reachable once a guest runs `nvme connect`
# against it, so what can be checked without a VM is the server side of it:
# that every documented option spelling starts a server with the namespace it
# asked for, that a file-backed namespace really appears on disk at the right
# size, and that a malformed option is refused before the guest ever attaches.
# The end-to-end connect lives in the VM system tests (ci/jobs/vm-nvmeof.sh).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
SERVER_BIN="${SERVER_BIN:-$BUILD_DIR/rocm-ernic}"
WORK_DIR="$(mktemp -d "/tmp/test-nvmeof-ci-$$-XXXXXX")"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Each entry is "<backend string>|<substring the startup log must contain>".
declare -a CONFIGS=(
    "nvmeof|nvmeof target 'nvmet-test' 67108864 bytes bs=512 at 192.168.200.1:4420"
    "nvmeof:size=16M|16777216 bytes bs=512"
    "nvmeof:size=1G,bs=4096|1073741824 bytes bs=4096"
    "nvmeof:size=1MiB,bs=1024|1048576 bytes bs=1024"
    "nvmeof:nqn=nqn.2026-01.com.amd:ernic|nvmeof target 'nqn.2026-01.com.amd:ernic'"
    "nvmeof:ip=10.0.0.5,port=4421|at 10.0.0.5:4421"
    "nvmeof:queues=1,size=4M|4194304 bytes"
    "nvmeof:size=8M,model=ernic-ns,serial=DEADBEEF|8388608 bytes"
)

# Option spellings that must be rejected before the server comes up.
declare -a BAD_CONFIGS=(
    "nvmeof:size=bogus"
    "nvmeof:bs=333"
    "nvmeof:size=1000,bs=512"
    "nvmeof:port=0"
    "nvmeof:queues=0"
    "nvmeof:queues=65"
    "nvmeof:ip=999.1.1.1"
    "nvmeof:nonsense=1"
)

declare -a PASSED=()
declare -a FAILED=()

# shellcheck disable=SC2317  # Called via trap
cleanup() {
    local pid="${SERVER_PID:-}"
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$WORK_DIR"
}

trap cleanup EXIT

if [ ! -f "$SERVER_BIN" ]; then
    echo -e "${RED}Error: Server binary not found: $SERVER_BIN${NC}"
    exit 1
fi

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  NVMe-oF Backend CI Testing                                ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo "Server binary: $SERVER_BIN"
echo "Configurations: ${#CONFIGS[@]} valid, ${#BAD_CONFIGS[@]} rejected"
echo ""
echo -e "${YELLOW}Note: the guest-side connect is covered by the VM system tests.${NC}"
echo ""

# Start a server, wait for its socket, and return its log path in SERVER_LOG.
start_server() {
    local config="$1"
    # A unix socket path is capped well below what these option strings run
    # to, so number the runs instead of naming them after the config.
    SERVER_SEQ=$((${SERVER_SEQ:-0} + 1))
    local socket_path="$WORK_DIR/s${SERVER_SEQ}.sock"

    SERVER_LOG="$WORK_DIR/s${SERVER_SEQ}.log"
    "$SERVER_BIN" \
        --socket "$socket_path" \
        --backend "$config" \
        --log-level info >"$SERVER_LOG" 2>&1 &
    SERVER_PID=$!

    for _ in {1..20}; do
        if [ -S "$socket_path" ]; then
            return 0
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            return 1
        fi
        sleep 0.25
    done
    return 1
}

stop_server() {
    kill "${SERVER_PID:-}" 2>/dev/null || true
    wait "${SERVER_PID:-}" 2>/dev/null || true
    SERVER_PID=""
}

test_config() {
    local config="${1%%|*}"
    local expect="${1#*|}"

    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Testing: ${config}${NC}"

    if ! start_server "$config"; then
        echo -e "${RED}✗ Server failed to start${NC}"
        tail -20 "$SERVER_LOG"
        FAILED+=("$config")
        stop_server
        return 1
    fi

    if ! grep -qF "$expect" "$SERVER_LOG"; then
        echo -e "${RED}✗ Startup log does not report: $expect${NC}"
        grep -i nvmeof "$SERVER_LOG" || tail -20 "$SERVER_LOG"
        FAILED+=("$config")
        stop_server
        return 1
    fi

    stop_server
    echo -e "${GREEN}✓ Controller reported: $expect${NC}"
    PASSED+=("$config")
    return 0
}

test_rejected() {
    local config="$1"

    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Testing rejection: ${config}${NC}"

    local out
    if out=$("$SERVER_BIN" --socket "$WORK_DIR/reject.sock" \
                 --backend "$config" 2>&1); then
        :
    fi

    if ! grep -qi "nvmeof backend:" <<<"$out"; then
        echo -e "${RED}✗ Bad option was not refused${NC}"
        echo "$out" | head -10
        FAILED+=("reject $config")
        return 1
    fi

    echo -e "${GREEN}✓ Refused: $(grep -i -m1 'nvmeof backend:' <<<"$out")${NC}"
    PASSED+=("reject $config")
    return 0
}

test_file_namespace() {
    local image="$WORK_DIR/ns0.img"
    local config="nvmeof:file=$image,size=32M,bs=4096"

    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Testing file-backed namespace${NC}"

    if ! start_server "$config"; then
        echo -e "${RED}✗ Server failed to start${NC}"
        tail -20 "$SERVER_LOG"
        FAILED+=("file-backed namespace")
        stop_server
        return 1
    fi
    stop_server

    local size
    size=$(stat -c %s "$image" 2>/dev/null || echo 0)
    if [ "$size" != "33554432" ]; then
        echo -e "${RED}✗ Namespace image is $size bytes, expected 33554432${NC}"
        FAILED+=("file-backed namespace")
        return 1
    fi

    echo -e "${GREEN}✓ Namespace image created at 32 MiB${NC}"
    PASSED+=("file-backed namespace")
    return 0
}

test_usage() {
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Testing usage text${NC}"

    local out
    out=$("$SERVER_BIN" --help 2>&1 || true)
    if ! grep -q "none|loopback|verbs|tcp|nvmeof" <<<"$out"; then
        echo -e "${RED}✗ nvmeof missing from the backend list${NC}"
        FAILED+=("usage text")
        return 1
    fi
    if ! grep -q "nvmeof:file=" <<<"$out"; then
        echo -e "${RED}✗ nvmeof options missing from the usage text${NC}"
        FAILED+=("usage text")
        return 1
    fi

    echo -e "${GREEN}✓ Usage text documents the backend${NC}"
    PASSED+=("usage text")
    return 0
}

for entry in "${CONFIGS[@]}"; do
    test_config "$entry" || true
    echo ""
done

for config in "${BAD_CONFIGS[@]}"; do
    test_rejected "$config" || true
    echo ""
done

test_file_namespace || true
echo ""
test_usage || true
echo ""

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  Test Summary                                               ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${GREEN}Passed: ${#PASSED[@]}${NC}"

if [ ${#FAILED[@]} -gt 0 ]; then
    echo -e "${RED}Failed: ${#FAILED[@]}${NC}"
    for name in "${FAILED[@]}"; do
        echo -e "  ${RED}✗${NC} $name"
    done
    echo ""
    echo -e "${RED}Some nvmeof backend tests failed!${NC}"
    exit 1
fi

echo ""
echo -e "${GREEN}All nvmeof backend tests passed!${NC}"
exit 0
