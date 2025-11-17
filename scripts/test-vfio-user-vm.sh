#!/bin/bash
#
# test-vfio-user-vm.sh
#
# End-to-end test script that:
# 1. Starts rocm_ernic server
# 2. Launches VM with vfio-user device
# 3. Provides instructions for testing inside the VM
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Configuration
SOCKET_PATH=${SOCKET_PATH:-/tmp/vfio-user-rocm-ernic-test.sock}
VM_NAME=${VM_NAME:-stebates-test-vm}
VFU_PVRDMA="${PROJECT_ROOT}/build/rocm_ernic"
SERVER_LOG="/tmp/rocm-ernic-server-$$.log"

# Cleanup function
cleanup() {
    echo ""
    echo "=== Cleanup ==="
    
    if [ -n "$SERVER_PID" ] && kill -0 $SERVER_PID 2>/dev/null; then
        echo "Stopping rocm_ernic server (PID $SERVER_PID)..."
        sudo kill $SERVER_PID 2>/dev/null || true
        wait $SERVER_PID 2>/dev/null || true
    fi
    
    rm -f "$SOCKET_PATH" "$SERVER_LOG"
    echo "Cleanup complete"
}

trap cleanup EXIT INT TERM

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  rocm_ernic + QEMU VM Test                                   ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# Check if rocm_ernic is built
if [ ! -x "$VFU_PVRDMA" ]; then
    echo "ERROR: rocm_ernic not found or not executable: $VFU_PVRDMA"
    echo "Build it first with: ninja -C build"
    exit 1
fi

# Check for InfiniBand device (optional but recommended)
echo "Checking for InfiniBand devices..."
IBDEV=$(ibv_devices 2>/dev/null | grep -v "device" | awk '{print $1}' | head -1 || echo "")

if [ -z "$IBDEV" ]; then
    echo "⚠  WARNING: No InfiniBand device found"
    echo "   Server will start but RDMA backend won't be functional"
    echo ""
    IB_ARGS=""
else
    echo "✓ Found IB device: $IBDEV"
    IB_ARGS="--device $IBDEV --port 1"
fi

# Start rocm_ernic server
echo ""
echo "=== Starting rocm_ernic Server ==="
echo "Socket: $SOCKET_PATH"
echo "Log:    $SERVER_LOG"
echo ""

sudo $VFU_PVRDMA --socket "$SOCKET_PATH" $IB_ARGS --verbose > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!

echo "Server started (PID $SERVER_PID)"
sleep 2

# Check if server is running
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "ERROR: Server died immediately. Check log:"
    cat "$SERVER_LOG"
    exit 1
fi

# Wait for socket
echo "Waiting for socket..."
for i in {1..10}; do
    if [ -S "$SOCKET_PATH" ]; then
        echo "✓ Socket ready"
        break
    fi
    sleep 0.5
done

if [ ! -S "$SOCKET_PATH" ]; then
    echo "ERROR: Socket not created"
    cat "$SERVER_LOG"
    exit 1
fi

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  Server Running - Ready to Launch VM                        ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "Server is running. You can now:"
echo ""
echo "1. In another terminal, launch the VM:"
echo "   cd $PROJECT_ROOT"
echo "   VFIO_USER_SOCKET=$SOCKET_PATH VM_NAME=$VM_NAME \\"
echo "     ./scripts/run-vm-vfio-user.sh"
echo ""
echo "2. Inside the VM, check for the PVRDMA device:"
echo "   lspci -nn | grep 15ad:0820"
echo "   sudo modprobe vmw_pvrdma"
echo "   ibv_devices"
echo ""
echo "3. To stop, press Ctrl-C here (will stop server and cleanup)"
echo ""
echo "Server log: tail -f $SERVER_LOG"
echo ""
echo "Press Ctrl-C to stop server and exit..."
echo ""

# Keep running
wait $SERVER_PID

