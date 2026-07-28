#!/bin/bash
# SPDX-License-Identifier: MIT
# Test script for running multiple TCP servers locally

set -e

TCP_PORT=5000
BUILD_DIR="./build"
SERVER_BIN="$BUILD_DIR/rocm-ernic"

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=== Testing TCP Backend with Multiple Servers ==="
echo ""

# Cleanup function
cleanup() {
    echo ""
    echo "Cleaning up..."
    # Kill processes if they exist
    if [ -n "$SERVER1_PID" ]; then
        kill "$SERVER1_PID" 2>/dev/null || true
    fi
    if [ -n "$SERVER2_PID" ]; then
        kill "$SERVER2_PID" 2>/dev/null || true
    fi
    if [ -n "$SERVER3_PID" ]; then
        kill "$SERVER3_PID" 2>/dev/null || true
    fi
    # Also kill any remaining rocm-ernic processes from this test
    pkill -f "rocm-ernic.*tcp.*server" 2>/dev/null || true
    sleep 1
    rm -f /tmp/vfio-user-server*.sock
    rm -f /tmp/tcp-test-server*.log
}

trap cleanup EXIT

# Check and kill any processes using the TCP port
check_port() {
    local port=$1
    local pid=""
    
    # Try different methods to find the process using the port
    if command -v lsof >/dev/null 2>&1; then
        pid=$(lsof -ti:$port 2>/dev/null || true)
    elif command -v ss >/dev/null 2>&1; then
        pid=$(ss -ltnp "sport = :$port" 2>/dev/null | grep -oP 'pid=\K[0-9]+' | head -1 || true)
    elif command -v netstat >/dev/null 2>&1; then
        pid=$(netstat -tlnp 2>/dev/null | grep ":$port " | grep -oP '[0-9]+/.*' | cut -d'/' -f1 | head -1 || true)
    fi
    
    if [ -n "$pid" ]; then
        echo -e "${YELLOW}Port $port is in use by PID $pid, killing...${NC}"
        kill $pid 2>/dev/null || true
        sleep 1
        # Check again
        pid=""
        if command -v lsof >/dev/null 2>&1; then
            pid=$(lsof -ti:$port 2>/dev/null || true)
        elif command -v ss >/dev/null 2>&1; then
            pid=$(ss -ltnp "sport = :$port" 2>/dev/null | grep -oP 'pid=\K[0-9]+' | head -1 || true)
        elif command -v netstat >/dev/null 2>&1; then
            pid=$(netstat -tlnp 2>/dev/null | grep ":$port " | grep -oP '[0-9]+/.*' | cut -d'/' -f1 | head -1 || true)
        fi
        if [ -n "$pid" ]; then
            echo -e "${RED}✗ Failed to free port $port (still in use by PID $pid)${NC}"
            exit 1
        fi
    fi
}

# Clean up any existing test processes and ports
echo "Checking for existing processes..."
check_port $TCP_PORT
pkill -f "rocm-ernic.*tcp.*server" 2>/dev/null || true
sleep 1
rm -f /tmp/vfio-user-server*.sock
rm -f /tmp/tcp-test-server*.log

# Check if server binary exists
if [ ! -f "$SERVER_BIN" ]; then
    echo -e "${RED}✗ Server binary not found: $SERVER_BIN${NC}"
    echo "Please build the project first: cmake --build build"
    exit 1
fi

# Start Manager (Server 1)
echo -e "${YELLOW}Starting Manager (Server 1) on port $TCP_PORT...${NC}"
rm -f /tmp/vfio-user-server1.sock
$SERVER_BIN \
    --socket /tmp/vfio-user-server1.sock \
    --backend "tcp:manager:listen:$TCP_PORT" \
    --verbose > /tmp/tcp-test-server1.log 2>&1 &
SERVER1_PID=$!

# Wait for manager socket
echo "Waiting for manager socket..."
for i in {1..10}; do
    if [ -S /tmp/vfio-user-server1.sock ]; then
        chmod 666 /tmp/vfio-user-server1.sock 2>/dev/null || true
        echo -e "${GREEN}✓ Manager started (PID: $SERVER1_PID)${NC}"
        break
    fi
    if [ $i -eq 10 ]; then
        echo -e "${RED}✗ Manager failed to start${NC}"
        cat /tmp/tcp-test-server1.log
        exit 1
    fi
    sleep 1
done

# Wait for manager to be ready
echo "Waiting for manager to be ready..."
for i in {1..30}; do
    if grep -q "Device realized, waiting for client connection" /tmp/tcp-test-server1.log; then
        echo -e "${GREEN}✓ Manager ready${NC}"
        break
    fi
    if [ $i -eq 30 ]; then
        echo -e "${RED}✗ Manager not ready after 30 seconds${NC}"
        tail -30 /tmp/tcp-test-server1.log
        exit 1
    fi
    sleep 1
done

# Start Worker 1 (Server 2)
echo ""
echo -e "${YELLOW}Starting Worker 1 (Server 2)...${NC}"
rm -f /tmp/vfio-user-server2.sock
$SERVER_BIN \
    --socket /tmp/vfio-user-server2.sock \
    --backend "tcp:worker:localhost:$TCP_PORT" \
    --verbose > /tmp/tcp-test-server2.log 2>&1 &
SERVER2_PID=$!

# Wait for worker 1 socket
echo "Waiting for worker 1 socket..."
for i in {1..10}; do
    if [ -S /tmp/vfio-user-server2.sock ]; then
        chmod 666 /tmp/vfio-user-server2.sock 2>/dev/null || true
        echo -e "${GREEN}✓ Worker 1 started (PID: $SERVER2_PID)${NC}"
        break
    fi
    if [ $i -eq 10 ]; then
        echo -e "${RED}✗ Worker 1 failed to start${NC}"
        cat /tmp/tcp-test-server2.log
        exit 1
    fi
    sleep 1
done

# Wait for worker 1 to register
echo "Waiting for worker 1 to register..."
for i in {1..30}; do
    if grep -q "Registered with manager" /tmp/tcp-test-server2.log; then
        echo -e "${GREEN}✓ Worker 1 registered${NC}"
        break
    fi
    if [ $i -eq 30 ]; then
        echo -e "${RED}✗ Worker 1 registration timeout${NC}"
        tail -50 /tmp/tcp-test-server2.log
        exit 1
    fi
    sleep 1
done

# Start Worker 2 (Server 3) - optional
echo ""
echo -e "${YELLOW}Starting Worker 2 (Server 3)...${NC}"
rm -f /tmp/vfio-user-server3.sock
$SERVER_BIN \
    --socket /tmp/vfio-user-server3.sock \
    --backend "tcp:worker:localhost:$TCP_PORT" \
    --verbose > /tmp/tcp-test-server3.log 2>&1 &
SERVER3_PID=$!

# Wait for worker 2 socket
echo "Waiting for worker 2 socket..."
for i in {1..10}; do
    if [ -S /tmp/vfio-user-server3.sock ]; then
        chmod 666 /tmp/vfio-user-server3.sock 2>/dev/null || true
        echo -e "${GREEN}✓ Worker 2 started (PID: $SERVER3_PID)${NC}"
        break
    fi
    if [ $i -eq 10 ]; then
        echo -e "${RED}✗ Worker 2 failed to start${NC}"
        cat /tmp/tcp-test-server3.log
        exit 1
    fi
    sleep 1
done

# Wait for worker 2 to register
echo "Waiting for worker 2 to register..."
for i in {1..30}; do
    if grep -q "Registered with manager" /tmp/tcp-test-server3.log; then
        echo -e "${GREEN}✓ Worker 2 registered${NC}"
        break
    fi
    if [ $i -eq 30 ]; then
        echo -e "${RED}✗ Worker 2 registration timeout${NC}"
        tail -50 /tmp/tcp-test-server3.log
        exit 1
    fi
    sleep 1
done

# Show status
echo ""
echo -e "${GREEN}=== All Servers Running ===${NC}"
echo "Manager PID: $SERVER1_PID"
echo "Worker 1 PID: $SERVER2_PID"
echo "Worker 2 PID: $SERVER3_PID"
echo ""
echo "Logs:"
echo "  Manager: /tmp/tcp-test-server1.log"
echo "  Worker 1: /tmp/tcp-test-server2.log"
echo "  Worker 2: /tmp/tcp-test-server3.log"
echo ""
echo "Sockets:"
ls -la /tmp/vfio-user-server*.sock
echo ""
echo -e "${YELLOW}Press Ctrl+C to stop all servers${NC}"

# Monitor logs
tail -f /tmp/tcp-test-server*.log
