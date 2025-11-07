#!/bin/bash
#
# test-driver.sh - Clean test of vfu_pvrdma server and driver
#

set -e

echo "=== VFU PVRDMA Driver Test Script ==="
echo ""

# Step 1: Clean up
echo "Step 1: Cleaning up old processes..."
sudo pkill -9 -f vfu_pvrdma 2>/dev/null || true
sudo pkill -9 -f qemu-system 2>/dev/null || true
sudo rm -f /tmp/vfio-user-pvrdma.sock
sleep 2
echo "✓ Cleanup complete"
echo ""

# Step 2: Start server
echo "Step 2: Starting vfu_pvrdma server..."
cd /home/stebates/Projects/vfu-rdma
sudo ./build/vfu_pvrdma --socket /tmp/vfio-user-pvrdma.sock --verbose 2>&1 | tee /tmp/vfu_server.log &
SERVER_PID=$!
sleep 3
sudo chmod 666 /tmp/vfio-user-pvrdma.sock

if ps -p $SERVER_PID > /dev/null; then
    echo "✓ Server started (PID: $SERVER_PID)"
else
    echo "✗ Server failed to start!"
    exit 1
fi
echo ""

# Step 3: Start VM
echo "Step 3: Starting VM..."
cd /home/stebates/Projects/qemu-minimal/qemu
sg kvm -c "./run-vm-vfio-user" &
VM_PID=$!
sleep 15
echo "✓ VM started (PID: $VM_PID)"
echo ""

# Step 4: Test driver
echo "Step 4: Testing driver in VM..."
ssh -o StrictHostKeyChecking=no -p 2222 ubuntu@localhost << 'EOFVM'
echo "Loading ib_uverbs..."
sudo modprobe ib_uverbs

echo "Loading amd_emrdma driver..."
cd /home/ubuntu/driver
sudo insmod amd_emrdma.ko

echo ""
echo "=== Driver Status ==="
lsmod | grep amd_emrdma

echo ""
echo "=== Kernel Messages ==="
sudo dmesg | grep amd_emrdma | tail -15
EOFVM

echo ""
echo "=== Server Log (last 30 lines) ==="
tail -30 /tmp/vfu_server.log

echo ""
echo "Test complete. VM is still running on port 2222"
echo "To connect: ssh -p 2222 ubuntu@localhost"
echo "To stop: sudo pkill -9 -f 'qemu-system|vfu_pvrdma'"

