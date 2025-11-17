#!/bin/bash
#
# Test rocm_ernic with non-blocking attach (nic-emu style)
#

set -e

echo "=== Cleanup ==="
sudo pkill -9 rocm_ernic 2>/dev/null || true
sudo pkill -9 qemu-system-x86 2>/dev/null || true
sudo rm -f /tmp/vfio-user-rocm-ernic.sock 2>/dev/null || true
sleep 1

echo ""
echo "=== Starting rocm_ernic server ==="
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"
sudo ./build/rocm_ernic -s /tmp/vfio-user-rocm-ernic.sock -v > /tmp/server.log 2>&1 &
SERVER_PID=$!
echo "Server PID: $SERVER_PID"

# Wait for socket
echo "Waiting for socket..."
for i in {1..20}; do
    if [ -S /tmp/vfio-user-rocm-ernic.sock ]; then
        sudo chmod 666 /tmp/vfio-user-rocm-ernic.sock
        echo "✓ Socket ready"
        break
    fi
    sleep 0.5
done

if [ ! -S /tmp/vfio-user-rocm-ernic.sock ]; then
    echo "✗ Socket not created"
    cat /tmp/server.log
    exit 1
fi

# Verify server is running
sleep 1
if ! ps -p $SERVER_PID > /dev/null 2>&1; then
    echo "✗ Server exited"
    cat /tmp/server.log
    exit 1
fi

echo "✓ Server is waiting for client"
echo ""
echo "=== Starting QEMU ==="
cd /home/stebates/Projects/qemu-minimal/qemu
./run-vm-vfio-user &
QEMU_PID=$!
echo "QEMU PID: $QEMU_PID"

echo ""
echo "=== Waiting for VM to boot ==="
sleep 10

echo ""
echo "=== Checking server connection ==="
if ps -p $SERVER_PID > /dev/null 2>&1; then
    echo "✓ Server still running"
    echo ""
    echo "Server log:"
    tail -20 /tmp/server.log
else
    echo "✗ Server crashed"
    cat /tmp/server.log
fi

echo ""
echo "=== To connect to VM: ssh -p 2222 ubuntu@localhost (password: ubuntu) ==="
echo "=== To stop: sudo pkill -9 qemu; sudo pkill -9 rocm_ernic ==="
echo ""
echo "Press Ctrl+C to stop..."
wait $QEMU_PID

