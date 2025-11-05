#!/bin/bash
#
# Test runner for vfu_pvrdma
#
# Usage: run-test.sh <test_client_path> <server_path>
#

set -e

# Arguments
TEST_CLIENT="$1"
SERVER="$2"
SOCKET_PATH="/tmp/vfio-user-pvrdma-test-$$.sock"
SERVER_LOG="/tmp/vfio-user-pvrdma-server-$$.log"

# Cleanup function
cleanup() {
    local exit_code=$?
    
    echo ""
    echo "=== Cleanup ==="
    
    # Kill server if running
    if [ -n "$SERVER_PID" ]; then
        echo "Stopping server (PID $SERVER_PID)..."
        kill $SERVER_PID 2>/dev/null || true
        wait $SERVER_PID 2>/dev/null || true
    fi
    
    # Remove socket and log
    rm -f "$SOCKET_PATH" "$SERVER_LOG"
    
    echo "Cleanup complete"
    exit $exit_code
}

# Set up cleanup trap
trap cleanup EXIT INT TERM

echo "================================================="
echo "  vfu_pvrdma Test Harness"
echo "================================================="
echo ""

# Validate arguments
if [ -z "$TEST_CLIENT" ] || [ -z "$SERVER" ]; then
    echo "Usage: $0 <test_client_path> <server_path>"
    exit 1
fi

if [ ! -x "$TEST_CLIENT" ]; then
    echo "Error: Test client not found or not executable: $TEST_CLIENT"
    exit 1
fi

if [ ! -x "$SERVER" ]; then
    echo "Error: Server not found or not executable: $SERVER"
    exit 1
fi

echo "Test Client: $TEST_CLIENT"
echo "Server:      $SERVER"
echo "Socket:      $SOCKET_PATH"
echo "Server Log:  $SERVER_LOG"
echo ""

# Start server in background
echo "=== Starting Server ==="
"$SERVER" --socket "$SOCKET_PATH" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
echo "Server started (PID $SERVER_PID)"

# Wait for server to be ready (socket created)
echo "Waiting for server to initialize..."
TIMEOUT=10
ELAPSED=0
while [ $ELAPSED -lt $TIMEOUT ]; do
    if [ -S "$SOCKET_PATH" ]; then
        echo "✓ Server socket ready"
        break
    fi
    
    # Check if server process is still running
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        echo "✗ Server process died unexpectedly"
        echo ""
        echo "=== Server Log ==="
        cat "$SERVER_LOG"
        exit 1
    fi
    
    sleep 0.5
    ELAPSED=$((ELAPSED + 1))
done

if [ $ELAPSED -ge $TIMEOUT ]; then
    echo "✗ Timeout waiting for server socket"
    echo ""
    echo "=== Server Log ==="
    cat "$SERVER_LOG"
    exit 1
fi

# Give server a moment to fully initialize
sleep 1

echo ""
echo "=== Running Tests ==="

# Run test client
if "$TEST_CLIENT" --socket "$SOCKET_PATH"; then
    TEST_RESULT="PASSED"
    TEST_EXIT=0
else
    TEST_RESULT="FAILED"
    TEST_EXIT=1
fi

echo ""
echo "=== Test Results ==="
echo "Status: $TEST_RESULT"
echo ""

# Show server log if test failed
if [ $TEST_EXIT -ne 0 ]; then
    echo "=== Server Log (last 50 lines) ==="
    tail -50 "$SERVER_LOG"
    echo ""
fi

exit $TEST_EXIT

