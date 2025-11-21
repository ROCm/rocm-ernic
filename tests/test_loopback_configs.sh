#!/bin/bash
# Test multiple loopback backend configurations
# This script tests rdma_cm emulation with different backend options

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
SERVER_BIN="$BUILD_DIR/rocm_ernic"
TEST_BIN="$BUILD_DIR/tests/test_rdma_cm"
SOCKET_PATH="/tmp/test-loopback-$$.sock"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test configurations
declare -a CONFIGS=(
    "loopback"
    "loopback:mode=preserve"
    "loopback:mode=zeros"
    "loopback:mode=random"
    "loopback:md5"
    "loopback:mode=preserve,md5"
)

# Cleanup function
cleanup() {
    local pid="${SERVER_PID:-}"
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -f "$SOCKET_PATH"
}

trap cleanup EXIT

# Check if binaries exist
if [ ! -f "$SERVER_BIN" ]; then
    echo -e "${RED}Error: Server binary not found: $SERVER_BIN${NC}"
    exit 1
fi

if [ ! -f "$TEST_BIN" ]; then
    echo -e "${RED}Error: Test binary not found: $TEST_BIN${NC}"
    exit 1
fi

echo "╔════════════════════════════════════════════════════════════╗"
echo "║  Testing Loopback Backend Configurations                    ║"
echo "║  RDMA Connection Manager (rdma_cm) Emulation Tests        ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

PASSED=0
FAILED=0
SKIPPED=0

for config in "${CONFIGS[@]}"; do
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "Testing configuration: $config"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    # Clean up any existing socket
    rm -f "$SOCKET_PATH"

    # Start server with this configuration
    echo "Starting server with: --backend $config"
    "$SERVER_BIN" \
        --socket "$SOCKET_PATH" \
        --backend "$config" \
        > "/tmp/server-$$.log" 2>&1 &
    SERVER_PID=$!

    # Wait for socket to be created
    TIMEOUT=10
    ELAPSED=0
    while [ ! -S "$SOCKET_PATH" ] && [ $ELAPSED -lt $TIMEOUT ]; do
        sleep 0.5
        ELAPSED=$((ELAPSED + 1))
    done

    if [ ! -S "$SOCKET_PATH" ]; then
        echo -e "${RED}✗ Server failed to create socket${NC}"
        echo "Server log:"
        cat "/tmp/server-$$.log"
        FAILED=$((FAILED + 1))
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        continue
    fi

    # Make socket accessible
    chmod 666 "$SOCKET_PATH" 2>/dev/null || true

    echo "✓ Server started (PID: $SERVER_PID)"

    # Wait a bit for server to fully initialize
    sleep 1

    # Run test
    echo "Running rdma_cm test..."
    if "$TEST_BIN" > "/tmp/test-$$.log" 2>&1; then
        TEST_EXIT=$?
        if [ $TEST_EXIT -eq 77 ]; then
            echo -e "${YELLOW}⚠ Test skipped (no RDMA device)${NC}"
            SKIPPED=$((SKIPPED + 1))
        elif [ $TEST_EXIT -eq 0 ]; then
            echo -e "${GREEN}✓ Test passed${NC}"
            PASSED=$((PASSED + 1))
        else
            echo -e "${RED}✗ Test failed (exit code: $TEST_EXIT)${NC}"
            echo "Test output:"
            cat "/tmp/test-$$.log"
            FAILED=$((FAILED + 1))
        fi
    else
        TEST_EXIT=$?
        if [ $TEST_EXIT -eq 77 ]; then
            echo -e "${YELLOW}⚠ Test skipped (no RDMA device)${NC}"
            SKIPPED=$((SKIPPED + 1))
        else
            echo -e "${RED}✗ Test failed (exit code: $TEST_EXIT)${NC}"
            echo "Test output:"
            cat "/tmp/test-$$.log"
            FAILED=$((FAILED + 1))
        fi
    fi

    # Stop server
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""

    # Clean up socket
    rm -f "$SOCKET_PATH"

    echo ""
done

# Summary
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Test Summary"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo -e "${GREEN}Passed:  $PASSED${NC}"
echo -e "${RED}Failed:  $FAILED${NC}"
echo -e "${YELLOW}Skipped: $SKIPPED${NC}"
echo "Total:   $((PASSED + FAILED + SKIPPED))"
echo ""

# Clean up log files
rm -f "/tmp/server-$$.log" "/tmp/test-$$.log"

if [ $FAILED -gt 0 ]; then
    exit 1
fi

exit 0

