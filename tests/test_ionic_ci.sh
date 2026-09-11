#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# CI test for the ionic emulation path, which is the default device mode.
# Verifies the server starts correctly across all backends and that the PCI
# device identity is correct.
#
# Tests that can run without a VM (no RDMA device needed):
#   1. Server starts in ionic mode with loopback backend
#   2. Server announces correct VID:DID (0x1022:0x8001)
#   3. Server reports correct BAR layout (64K BAR0 / 32K regs + 4M BAR2)
#   4. Server reports correct MSI-X vector count (32)
#   5. Server exits cleanly on SIGTERM
#   6. --tap is rejected with --legacy, and attaches when a tap exists
#   7. ionic is the default with no flag; --legacy selects the deprecated
#      PVRDMA device and warns

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
    rm -f "$SOCKET" "$LOG" ${STATS:+"$STATS"}
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
# BAR0 is 64K: a 32K ionic register window (ionic_dev_setup() requires
# >= IONIC_BAR0_SIZE = 0x8000) plus the MSI-X table/PBA above it, which
# cannot alias the register window.  BAR2 is the 4M doorbell BAR.
echo ""
echo "Test 4: BAR layout (BAR0=64K total / 32K regs, BAR2=4M)"
grep -q "BAR0=65536" "$LOG" || fail "BAR0 size wrong (expected 65536=64K)"
grep -q "regs=32768" "$LOG" || fail "BAR0 register window wrong (expected 32768=32K)"
grep -q "BAR2=4194304" "$LOG" || fail "BAR2 size wrong (expected 4194304=4M)"
pass "BAR layout correct"

# --- Test 5: correct MSI-X vector count ---
echo ""
echo "Test 5: MSI-X 32 vectors"
grep -q "MSI-X=32 vectors" "$LOG" || fail "MSI-X vector count wrong (expected 32)"
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

# --- Test 8: --tap is rejected in legacy mode ---
echo ""
echo "Test 8: --tap with --legacy is rejected"
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""
rm -f "$SOCKET" "$LOG"
if "$SERVER_BIN" --legacy --backend none --socket "$SOCKET" --tap ernic-ci0 \
        > "$LOG" 2>&1; then
    fail "--tap with --legacy should have failed"
fi
grep -q -- "--tap is not supported with --legacy" "$LOG" || {
    cat "$LOG"
    fail "expected a '--tap is not supported with --legacy' diagnostic"
}
pass "--tap rejected in legacy mode"

# --- Test 9: --tap attaches to a host TAP interface ---
# Needs a pre-created persistent tap owned by this user; skipped otherwise,
# because creating one takes CAP_NET_ADMIN that CI runners rarely grant.
echo ""
echo "Test 9: --tap attaches when the interface exists"
TAP_IF="${ERNIC_TEST_TAP:-}"
if [ -z "$TAP_IF" ] || [ ! -d "/sys/class/net/$TAP_IF" ]; then
    echo -e "${YELLOW}⚠ skipped: set ERNIC_TEST_TAP to a tap owned by $USER${NC}"
else
    rm -f "$SOCKET" "$LOG"
    "$SERVER_BIN" --ionic --backend loopback --socket "$SOCKET" \
        --tap "$TAP_IF" > "$LOG" 2>&1 &
    SERVER_PID=$!
    elapsed=0
    while [ $elapsed -lt 10 ]; do
        sleep 0.5; elapsed=$((elapsed + 1))
        [ -S "$SOCKET" ] && break
        kill -0 "$SERVER_PID" 2>/dev/null || { cat "$LOG"; fail "server died"; }
    done
    grep -q "Ethernet attached to TAP $TAP_IF" "$LOG" || {
        cat "$LOG"
        fail "server did not report attaching to $TAP_IF"
    }
    pass "attached to TAP $TAP_IF"
fi

# --- Test 10: ionic is the default with no mode flag ---
echo ""
echo "Test 10: ionic is the default (no flag)"
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""
rm -f "$SOCKET" "$LOG"
"$SERVER_BIN" --backend loopback --socket "$SOCKET" > "$LOG" 2>&1 &
SERVER_PID=$!
elapsed=0
while [ $elapsed -lt 10 ]; do
    sleep 0.5; elapsed=$((elapsed + 1))
    [ -S "$SOCKET" ] && break
    kill -0 "$SERVER_PID" 2>/dev/null || { cat "$LOG"; fail "server died"; }
done
grep -q "VID:DID 0x1022:0x8001" "$LOG" || {
    cat "$LOG"
    fail "default mode did not announce the ionic device"
}
pass "default mode is ionic"

# --- Test 11: --legacy selects the deprecated PVRDMA device ---
echo ""
echo "Test 11: --legacy selects 0x1022:0x8000 and warns"
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""
rm -f "$SOCKET" "$LOG"
"$SERVER_BIN" --legacy --backend loopback --socket "$SOCKET" > "$LOG" 2>&1 &
SERVER_PID=$!
elapsed=0
while [ $elapsed -lt 10 ]; do
    sleep 0.5; elapsed=$((elapsed + 1))
    [ -S "$SOCKET" ] && break
    kill -0 "$SERVER_PID" 2>/dev/null || { cat "$LOG"; fail "server died"; }
done
grep -q "device=0x8000" "$LOG" || {
    cat "$LOG"
    fail "--legacy did not configure the PVRDMA device id"
}
grep -qi "deprecated" "$LOG" || {
    cat "$LOG"
    fail "--legacy did not print a deprecation warning"
}
pass "--legacy works and is marked deprecated"

# --- Test 12: stats file is emitted in ionic mode with the full counter set ---
# The counters themselves stay zero here: driving them needs a real vfio-user
# client, which this VM-free suite does not have.  What this does guard is that
# --stats-file works in ionic mode and that the field set ionic feeds is the
# same one ernicctl parses.
echo ""
echo "Test 12: stats file carries the full counter set in ionic mode"
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""
rm -f "$SOCKET" "$LOG"
STATS="/tmp/vfio-ionic-ci-$$.stats"
rm -f "$STATS"
"$SERVER_BIN" --backend loopback --socket "$SOCKET" --stats-file "$STATS" \
    > "$LOG" 2>&1 &
SERVER_PID=$!
elapsed=0
while [ $elapsed -lt 10 ]; do
    sleep 0.5; elapsed=$((elapsed + 1))
    [ -s "$STATS" ] && break
    kill -0 "$SERVER_PID" 2>/dev/null || { cat "$LOG"; fail "server died"; }
done
[ -s "$STATS" ] || fail "no stats file was written"
for field in commands bar0_reads bar0_writes uar_reads uar_writes \
             mmio_reads_total mmio_writes_total interrupts reset_count \
             total_bytes_sent total_bytes_received total_bytes_rdma_read \
             total_bytes_rdma_write total_ip_bytes_tx total_ip_bytes_rx; do
    grep -q "^  $field  *: " "$STATS" || {
        cat "$STATS"
        fail "stats file is missing the '$field' counter"
    }
done
rm -f "$STATS"
pass "stats file has every counter ernicctl expects"

echo ""
echo "================================================="
echo -e "${GREEN}All ionic CI tests passed.${NC}"
echo "================================================="
