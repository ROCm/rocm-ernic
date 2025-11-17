#!/bin/bash
#
# verify-version-init.sh - Verify version register is initialized
#
# This script starts the server and checks logs to confirm
# the version register is properly initialized to 17.

set -e

SERVER="$1"
SOCKET="/tmp/vfio-version-test-$$.sock"
LOG="/tmp/vfio-version-test-$$.log"

if [ -z "$SERVER" ]; then
    echo "Usage: $0 <server-binary>"
    exit 1
fi

if [ ! -x "$SERVER" ]; then
    echo "Error: Server binary not found or not executable: $SERVER"
    exit 1
fi

cleanup() {
    sudo pkill -9 -f "$SOCKET" 2>/dev/null || true
    rm -f "$SOCKET" "$LOG"
}

trap cleanup EXIT

echo "=========================================="
echo " Version Register Initialization Test"
echo "=========================================="
echo ""

# Start server
echo "[1] Starting server..."
sudo "$SERVER" --socket "$SOCKET" > "$LOG" 2>&1 &
SERVER_PID=$!

sleep 3

# Check if still running
if ! ps -p $SERVER_PID > /dev/null 2>&1; then
    echo "    ✗ Server crashed during startup"
    echo ""
    echo "Server log:"
    cat "$LOG"
    exit 1
fi

echo "    ✓ Server started (PID $SERVER_PID)"

# Check for socket
if [ ! -S "$SOCKET" ]; then
    echo "    ✗ Socket not created"
    exit 1
fi

echo "    ✓ Socket created"
echo ""

# Parse log for key messages
echo "[2] Checking initialization logs..."

if grep -q "PVRDMA device created" "$LOG"; then
    echo "    ✓ Device created"
else
    echo "    ✗ Device not created"
    cat "$LOG"
    exit 1
fi

if grep -q "PVRDMA version register initialized to 17" "$LOG"; then
    echo "    ✓ Version register initialized to 17"
else
    echo "    ✗ Version register not initialized"
    echo ""
    echo "Searching for version messages:"
    grep -i version "$LOG" || echo "    (none found)"
    exit 1
fi

if grep -q "PVRDMA device realized successfully" "$LOG"; then
    echo "    ✓ Device realized successfully"
else
    echo "    ✗ Device not realized"
    cat "$LOG"
    exit 1
fi

echo ""
echo "[3] Checking for errors..."

if grep -q "Failed to realize PVRDMA device" "$LOG"; then
    echo "    ✗ Realization failed"
    grep "Failed" "$LOG"
    exit 1
fi

# RDMA backend failure is expected without IB hardware
if grep -q "RDMA backend initialization failed" "$LOG"; then
    echo "    ⚠ RDMA backend not available (expected without IB hardware)"
else
    echo "    ✓ RDMA backend initialized (IB hardware present)"
fi

echo ""
echo "=========================================="
echo "✓ ALL CHECKS PASSED"
echo "=========================================="
echo ""
echo "Summary:"
echo "  • Server starts successfully"
echo "  • Device creation succeeds"  
echo "  • Version register = 17"
echo "  • Device realization succeeds"
echo ""
echo "Next step:"
echo "  Test with QEMU to verify guest driver can read version=17"
echo ""

exit 0

