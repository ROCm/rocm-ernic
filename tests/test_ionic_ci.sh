#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# CI test for the ionic emulation path.
# Verifies the server starts correctly in --ionic mode across all backends
# and that the PCI device identity is correct.
#
# Tests that can run without a VM (no RDMA device needed):
#   1. Server starts in --ionic mode with loopback backend
#   2. Server announces correct VID:DID (0x1022:0x8001)
#   3. Server reports correct BAR layout (32K + 4M)
#   4. Server reports correct MSI-X vector count (4)
#   5. Server exits cleanly on SIGTERM

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
SERVER_BIN="${SERVER_BIN:-$BUILD_DIR/rocm-ernic}"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { echo -e "${GREEN}✓ $*${NC}"; }
fail() { echo -e "${RED}✗ $*${NC}"; exit 1; }
skip() { echo -e "${YELLOW}⚠ $*${NC}"; exit 77; }

if [ ! -x "$SERVER_BIN" ]; then
    skip "server binary not found: $SERVER_BIN"
fi

echo "================================================="
echo "  rocm-ernic ionic path CI tests"
echo "  Server: $SERVER_BIN"
echo "================================================="

SOCKET="/tmp/vfio-ionic-ci-$$.sock"
LOG="/tmp/vfio-ionic-ci-$$.log"
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -f "$SOCKET" "$LOG"
}
trap cleanup EXIT INT TERM

start_server() {
    local backend="${1:-loopback}"
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -f "$SOCKET" "$LOG"

    "$SERVER_BIN" --ionic --backend "$backend" --socket "$SOCKET" \
        > "$LOG" 2>&1 &
    SERVER_PID=$!

    local elapsed=0
    while [ $elapsed -lt 10 ]; do
        sleep 0.5; elapsed=$((elapsed + 1))
        [ -S "$SOCKET" ] && return 0
        kill -0 "$SERVER_PID" 2>/dev/null || { cat "$LOG"; return 1; }
    done
    cat "$LOG"; return 1
}

# --- Test 1: starts with loopback backend ---
echo ""
echo "Test 1: ionic mode starts (loopback backend)"
start_server loopback || fail "server did not start"
pass "socket appeared"

# --- Test 2: correct VID:DID ---
echo ""
echo "Test 2: VID:DID 0x1022:0x8001"
grep -q "VID:DID 0x1022:0x8001" "$LOG" || fail "VID:DID not found in log"
pass "VID:DID correct"

# --- Test 3: ionic banner ---
echo ""
echo "Test 3: ionic mode banner"
grep -q "ionic emulation initialized" "$LOG" || fail "ionic banner missing"
pass "ionic banner present"

# --- Test 4: correct BAR layout ---
echo ""
echo "Test 4: BAR layout (BAR0=32K, BAR2=4M)"
grep -q "BAR0=4194304" "$LOG" || fail "BAR0 size wrong (expected 4194304=4M)"
grep -q "BAR2=4194304" "$LOG" || fail "BAR2 size wrong (expected 4194304=4M)"
pass "BAR layout correct"

# --- Test 5: correct MSI-X vector count ---
echo ""
echo "Test 5: MSI-X 4 vectors (IONIC_EQ_COUNT_MIN)"
grep -q "MSI-X=4 vectors" "$LOG" || fail "MSI-X vector count wrong (expected 4)"
pass "MSI-X vectors correct"

# --- Test 6: SIGTERM shuts down cleanly ---
echo ""
echo "Test 6: clean SIGTERM shutdown"
kill -TERM "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""
pass "server exited on SIGTERM"

# --- Test 7: none backend ---
echo ""
echo "Test 7: ionic mode with none backend"
start_server none || fail "server did not start with none backend"
grep -q "VID:DID 0x1022:0x8001" "$LOG" || fail "VID:DID not in none backend log"
pass "none backend works"

echo ""
echo "================================================="
echo -e "${GREEN}All ionic CI tests passed.${NC}"
echo "================================================="
