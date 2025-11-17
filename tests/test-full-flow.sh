#!/bin/bash
#
# Full test: Server startup -> QEMU boot -> Driver load
#

set -e

echo "=== Full Flow Test ==="
echo ""

# Cleanup
echo "[1/5] Cleanup old processes..."
sudo pkill -9 rocm_ernic 2>/dev/null || true
sudo pkill -9 qemu-system-x86 2>/dev/null || true
sudo rm -f /tmp/vfio-user-rocm-ernic.sock 2>/dev/null || true
sleep 1

# Start server with loopback backend
echo "[2/5] Starting rocm_ernic server (loopback backend)..."
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"
sudo ./build/rocm_ernic -s /tmp/vfio-user-rocm-ernic.sock --backend loopback -v > /tmp/server.log 2>&1 &
SERVER_PID=$!

# Wait for socket
for i in {1..20}; do
    if [ -S /tmp/vfio-user-rocm-ernic.sock ]; then
        sudo chmod 666 /tmp/vfio-user-rocm-ernic.sock
        break
    fi
    sleep 0.5
done

if [ ! -S /tmp/vfio-user-rocm-ernic.sock ]; then
    echo "✗ Server failed to create socket"
    cat /tmp/server.log
    exit 1
fi

echo "✓ Server running (PID: $SERVER_PID)"

# Start QEMU
echo "[3/5] Starting QEMU..."
cd /home/stebates/Projects/qemu-minimal/qemu
sudo VM_NAME=vfu-rdma-test SSH_PORT=2222 VFIO_SOCKET=/tmp/vfio-user-rocm-ernic.sock \
    ./run-vm-vfio-user > /tmp/vm.log 2>&1 &
QEMU_PID=$!

echo "✓ QEMU starting (PID: $QEMU_PID)"

# Wait for VM boot
echo "[4/5] Waiting for VM to boot..."
sleep 15

# Check if VM is accessible
echo "[5/5] Testing VM connectivity..."
for i in {1..10}; do
    if timeout 2 ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -p 2222 ubuntu@localhost "echo 'VM Ready'" 2>/dev/null; then
        echo "✓ VM is accessible via SSH"
        break
    fi
    sleep 2
done

echo ""
echo "=== Status ==="
echo "Server: $(ps -p $SERVER_PID > /dev/null 2>&1 && echo 'Running' || echo 'Stopped')"
echo "QEMU: $(ps -p $QEMU_PID > /dev/null 2>&1 && echo 'Running' || echo 'Stopped')"
echo ""
echo "=== Server Log (last 20 lines) ==="
tail -20 /tmp/server.log
echo ""
echo "=== To access VM ==="
echo "ssh -p 2222 ubuntu@localhost  (password: ubuntu)"
echo ""
echo "=== To test driver in VM ==="
echo "1. SSH into VM"
echo "2. cd /tmp/driver && make clean && make"
echo "3. sudo modprobe ib_uverbs"
echo "4. sudo insmod amd_emrdma.ko"
echo "5. dmesg | tail -30"
echo ""
echo "=== To cleanup ==="
echo "sudo pkill -9 qemu-system-x86; sudo pkill -9 rocm_ernic"
echo ""
echo "Press Ctrl+C to stop monitoring (VM will keep running)"
tail -f /tmp/server.log

