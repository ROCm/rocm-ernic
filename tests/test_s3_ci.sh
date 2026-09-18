#!/bin/bash
# SPDX-License-Identifier: MIT
# S3-over-RDMA backend testing for CI.
#
# The object store only becomes reachable once a guest drives HTTP at the
# emulated NIC, so what can be checked without a VM is the server side of it:
# that every documented option spelling starts a server with the store it
# asked for, that the in-band endpoint announces the address and the derived
# MAC the guest will have to ARP for, and that a malformed option is refused
# before the guest ever attaches. The end-to-end PUT/GET over RDMA lives in
# the VM system tests (ci/jobs/vm-s3.sh).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
SERVER_BIN="${SERVER_BIN:-$BUILD_DIR/rocm-ernic}"
WORK_DIR="$(mktemp -d "/tmp/test-s3-ci-$$-XXXXXX")"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Each entry is "<backend string>|<substring the startup log must contain>".
declare -a CONFIGS=(
    "s3|s3 bucket 'ernic' 268435456 bytes at http://192.168.200.1:9000/"
    "s3:size=16M|16777216 bytes"
    "s3:size=1G|1073741824 bytes"
    "s3:size=1MiB|1048576 bytes"
    "s3:bucket=bench|s3 bucket 'bench'"
    "s3:ip=10.0.0.5,port=8080|at http://10.0.0.5:8080/"
    "s3:objects=4,size=4M|4194304 bytes"
    "s3:bucket=data,size=8M,maxpart=1M|s3 bucket 'data' 8388608 bytes"
)

# Option spellings that must be rejected before the server comes up.
declare -a BAD_CONFIGS=(
    "s3:size=bogus"
    "s3:bucket="
    "s3:bucket=No_Caps"
    "s3:port=0"
    "s3:port=99999"
    "s3:objects=0"
    "s3:ip=999.1.1.1"
    "s3:nonsense=1"
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
echo -e "${BLUE}║  S3-over-RDMA Backend CI Testing                           ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo "Server binary: $SERVER_BIN"
echo "Configurations: ${#CONFIGS[@]} valid, ${#BAD_CONFIGS[@]} rejected"
echo ""
echo -e "${YELLOW}Note: the guest-side PUT/GET is covered by the VM system tests.${NC}"
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
        grep -i "s3" "$SERVER_LOG" || tail -20 "$SERVER_LOG"
        FAILED+=("$config")
        stop_server
        return 1
    fi

    stop_server
    echo -e "${GREEN}✓ Store reported: $expect${NC}"
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

    if ! grep -qi "s3 backend:" <<<"$out"; then
        echo -e "${RED}✗ Bad option was not refused${NC}"
        echo "$out" | head -10
        FAILED+=("reject $config")
        return 1
    fi

    echo -e "${GREEN}✓ Refused: $(grep -i -m1 's3 backend:' <<<"$out")${NC}"
    PASSED+=("reject $config")
    return 0
}

# The guest finds the endpoint by ARP, so the MAC the server derives from
# the configured address is part of the contract and not an internal detail.
test_endpoint_mac() {
    local config="s3:ip=192.168.200.1"

    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Testing endpoint MAC derivation${NC}"

    if ! start_server "$config"; then
        echo -e "${RED}✗ Server failed to start${NC}"
        tail -20 "$SERVER_LOG"
        FAILED+=("endpoint mac")
        stop_server
        return 1
    fi
    stop_server

    # 02:53 ('S') followed by the four address bytes.
    if ! grep -qF "mac 02:53:c0:a8:c8:01" "$SERVER_LOG"; then
        echo -e "${RED}✗ Endpoint MAC is not derived from the address${NC}"
        grep -i "s3" "$SERVER_LOG" || tail -20 "$SERVER_LOG"
        FAILED+=("endpoint mac")
        return 1
    fi

    echo -e "${GREEN}✓ Endpoint answers ARP as 02:53:c0:a8:c8:01${NC}"
    PASSED+=("endpoint mac")
    return 0
}

test_usage() {
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Testing usage text${NC}"

    local out
    out=$("$SERVER_BIN" --help 2>&1 || true)
    if ! grep -q "none|loopback|verbs|tcp|nvmeof|s3" <<<"$out"; then
        echo -e "${RED}✗ s3 missing from the backend list${NC}"
        FAILED+=("usage text")
        return 1
    fi
    if ! grep -q "s3:bucket=" <<<"$out"; then
        echo -e "${RED}✗ s3 options missing from the usage text${NC}"
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

test_endpoint_mac || true
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
    echo -e "${RED}Some s3 backend tests failed!${NC}"
    exit 1
fi

echo ""
echo -e "${GREEN}All s3 backend tests passed!${NC}"
exit 0
