#!/bin/bash
set -e

echo "=== CQ Creation Test Script ==="
echo

# Kill any existing processes
echo "Cleaning up old processes..."
sudo pkill -9 rocm_ernic 2>/dev/null || true
sudo pkill -9 qemu-system 2>/dev/null || true
sleep 3

# Remove old socket
sudo rm -f /tmp/vfio-user-rocm-ernic.sock

# Start server
echo "Starting rocm_ernic server..."
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"
sudo ./build/rocm_ernic --backend loopback:random,md5 -v > /tmp/test-cq.log 2>&1 &
SERVER_PID=$!
echo "  Server PID: $SERVER_PID"
sleep 3

if ! ps -p $SERVER_PID > /dev/null; then
    echo "  ✗ Server failed to start!"
    sudo tail -20 /tmp/test-cq.log
    exit 1
fi
echo "  ✓ Server running"

# Start VM
echo "Starting QEMU VM..."
cd /home/stebates/Projects/qemu-minimal/qemu
sudo ./run-vm-vfio-user > /tmp/test-cq-qemu.log 2>&1 &
VM_PID=$!
echo "  VM PID: $VM_PID"
echo "  Waiting for boot (25 seconds)..."
sleep 25

if ! ps -p $VM_PID > /dev/null; then
    echo "  ✗ VM failed to start!"
    sudo tail -20 /tmp/test-cq-qemu.log
    exit 1
fi
echo "  ✓ VM running"

# Load driver in VM
echo "Loading driver in VM..."
timeout 60 ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -p 2222 ubuntu@localhost << 'EOFVM' 2>&1 | grep -v "Warning:"
sudo modprobe ib_uverbs
sudo insmod /mnt/host/driver/amd_emrdma.ko
echo "✓ Driver loaded"
EOFVM

echo
echo "=== Checking Results ==="

# Check server logs
echo "Server logs (last 30 lines with CQ/DSR activity):"
sudo grep -E "create_cq|DSR|command = 6" /tmp/test-cq.log | tail -30 || echo "  No CQ activity yet"

echo
echo "Driver dmesg:"
timeout 30 ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p 2222 ubuntu@localhost "sudo dmesg | grep -E 'CQ create|Couldn't create|max_cqe' | tail -5" 2>&1 | grep -v "Warning:"

echo
echo "=== Test Complete ==="
echo "Server log: /tmp/test-cq.log"
echo "QEMU log: /tmp/test-cq-qemu.log"

