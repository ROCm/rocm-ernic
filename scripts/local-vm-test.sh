#!/bin/bash
# SPDX-License-Identifier: MIT
#
# local-vm-test.sh
#
# Comprehensive local testing script for rocm-ernic with a VM
# This mimics the CI integration test but runs locally
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Configuration
QEMU_PATH=${QEMU_PATH:-/opt/qemu-v10.1.2/bin/}
QEMU_MINIMAL=${QEMU_MINIMAL:-$HOME/Projects/qemu-minimal}
SOCKET_PATH="/tmp/vfio-user-rocm-ernic.sock"
SERVER_LOG="/tmp/rocm-ernic-server.log"
VM_NAME=${VM_NAME:-stebates-test-vm}  # Use your existing VM by default

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

function log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

function log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

function log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

function cleanup() {
    log_info "Cleaning up..."
    
    # Stop VM if running
    if pgrep -f "qemu.*${VM_NAME}" > /dev/null; then
        log_info "Stopping VM..."
        pkill -f "qemu.*${VM_NAME}" || true
        sleep 2
    fi
    
    # Stop server if running
    if [ -f /tmp/rocm-ernic-server.pid ]; then
        PID=$(cat /tmp/rocm-ernic-server.pid)
        if ps -p $PID > /dev/null 2>&1; then
            log_info "Stopping rocm-ernic server (PID: $PID)..."
            sudo kill $PID || true
            sleep 1
        fi
        rm -f /tmp/rocm-ernic-server.pid
    fi
    
    # Remove socket
    sudo rm -f ${SOCKET_PATH}
    
    log_info "Cleanup complete"
}

trap cleanup EXIT

# Check prerequisites
log_info "Checking prerequisites..."

if [ ! -d "$PROJECT_ROOT/build" ]; then
    log_error "Build directory not found. Please run: cmake -B build -G Ninja && cmake --build build"
    exit 1
fi

if [ ! -f "$PROJECT_ROOT/build/rocm-ernic" ]; then
    log_error "rocm-ernic executable not found. Please run: ninja -C build"
    exit 1
fi

if [ ! -d "$QEMU_MINIMAL" ]; then
    log_error "qemu-minimal not found at $QEMU_MINIMAL"
    log_error "Please set QEMU_MINIMAL environment variable"
    exit 1
fi

if [ ! -f "${QEMU_PATH}/qemu-system-x86_64" ]; then
    log_error "QEMU 10.1+ not found at $QEMU_PATH"
    log_error "Please install QEMU 10.1.2+ or set QEMU_PATH"
    exit 1
fi

log_info "✓ All prerequisites met"

# Build the server if needed
log_info "Building rocm-ernic server..."
cd "$PROJECT_ROOT"
ninja -C build
log_info "✓ Build complete"

# Check for InfiniBand device (optional for loopback)
log_info "Checking for InfiniBand devices..."
# Get device list, skip header lines (they contain "device" and "----")
IB_DEVICE=$(ibv_devices 2>/dev/null | tail -n +3 | grep -v "^$" | head -1 | awk '{print $1}')

if [ -n "$IB_DEVICE" ] && [ "$IB_DEVICE" != "device" ] && [ "$IB_DEVICE" != "------" ]; then
    log_info "✓ Found IB device: $IB_DEVICE"
    log_warn "Verbs backend not fully implemented yet, using loopback"
    BACKEND_ARGS="--backend loopback"
    # Future: BACKEND_ARGS="--backend verbs:${IB_DEVICE}"
else
    log_info "No IB devices found, using loopback backend"
    BACKEND_ARGS="--backend loopback"
fi

# Start rocm-ernic server
log_info "Starting rocm-ernic server..."
log_info "Socket: $SOCKET_PATH"
log_info "Backend: $BACKEND_ARGS"
log_info "Log: $SERVER_LOG"

sudo rm -f ${SOCKET_PATH}
sudo ${PROJECT_ROOT}/build/rocm-ernic \
    --socket ${SOCKET_PATH} \
    ${BACKEND_ARGS} \
    > ${SERVER_LOG} 2>&1 &

SERVER_PID=$!
echo $SERVER_PID > /tmp/rocm-ernic-server.pid
log_info "Server started (PID: $SERVER_PID)"

# Wait for socket to be created
log_info "Waiting for vfio-user socket..."
for i in {1..30}; do
    if [ -S "${SOCKET_PATH}" ]; then
        log_info "✓ Socket created"
        break
    fi
    sleep 0.5
done

if [ ! -S "${SOCKET_PATH}" ]; then
    log_error "Socket not created after 15 seconds"
    log_error "Server log:"
    cat ${SERVER_LOG}
    exit 1
fi

# Give server a moment to fully initialize
sleep 2

# Check if server is still running
if ! ps -p $SERVER_PID > /dev/null; then
    log_error "Server crashed during startup"
    log_error "Server log:"
    cat ${SERVER_LOG}
    exit 1
fi

log_info "✓ Server running and ready"

# Start VM with vfio-user device (in background)
log_info "Starting VM with vfio-user device (background)..."

cd "$QEMU_MINIMAL"

RUN_VM="${QEMU_MINIMAL}/qemu/run-vm"

# Set environment for run-vm
export QEMU_PATH=${QEMU_PATH}
export VM_NAME=${VM_NAME}
export VFIO_USERDEV=${SOCKET_PATH}
export QMP_SOCKET="true"
export SSH_PORT=2222
export KVM=enable
export VCPUS=4
export VMEM=8192

log_info "VM Configuration:"
log_info "  Name: $VM_NAME"
log_info "  QEMU: $QEMU_PATH"
log_info "  vfio-user socket: $SOCKET_PATH"
log_info "  SSH port: 2222"

if [ ! -f "${RUN_VM}" ]; then
    log_error "qemu-minimal run-vm not found: ${RUN_VM}"
    exit 1
fi

# Start VM in background
log_info "Launching VM..."
${RUN_VM} > /tmp/vm-output.log 2>&1 &
VM_PID=$!
log_info "VM started (PID: $VM_PID)"

# Wait for SSH to be available
log_info "Waiting for VM SSH (this may take 30-60 seconds)..."
SSH_READY=false
for i in {1..60}; do
    if ssh -o StrictHostKeyChecking=no -o ConnectTimeout=2 -p 2222 stebates@localhost 'echo VM Ready' 2>/dev/null; then
        SSH_READY=true
        log_info "✓ VM SSH is ready (attempt $i/60)"
        break
    fi
    sleep 2
done

if [ "$SSH_READY" != "true" ]; then
    log_error "VM SSH not ready after 120 seconds"
    log_error "VM output:"
    tail -50 /tmp/vm-output.log
    exit 1
fi

# Copy driver source to VM
log_info "Copying driver source to VM..."
scp -o StrictHostKeyChecking=no -P 2222 -r ${PROJECT_ROOT}/driver stebates@localhost:/tmp/ || {
    log_error "Failed to copy driver to VM"
    exit 1
}
log_info "✓ Driver source copied"

# Run automated tests in VM
log_info ""
log_info "==================================================================="
log_info "Running automated tests in VM..."
log_info "==================================================================="

ssh -o StrictHostKeyChecking=no -p 2222 stebates@localhost << 'EOFVM'
set -e

echo "=== Kernel Version ==="
uname -r

echo ""
echo "=== PCI Devices ==="
lspci | grep -i amd || echo "AMD device not found"

echo ""
echo "=== Loading InfiniBand Core Modules ==="
sudo modprobe ib_core
sudo modprobe ib_uverbs
lsmod | grep ib_ | head -5

echo ""
echo "=== Building Driver ==="
cd /tmp/driver
make
ls -lh rocm_ernic.ko

echo ""
echo "=== Loading Driver ==="
sudo insmod ./rocm_ernic.ko || {
    echo "Failed to load driver"
    sudo dmesg | tail -30
    exit 1
}

echo ""
echo "=== Verifying Driver Loaded ==="
lsmod | grep rocm_ernic

echo ""
echo "=== Kernel Messages ==="
sudo dmesg | grep -i "rocm_ernic" | tail -20 || echo "No rocm_ernic messages"

echo ""
echo "=== RDMA Devices ==="
ibv_devices || echo "No RDMA devices found"

echo ""
echo "=== Device Info ==="
# Check for device (driver name is rocm_ernic, device name is rocep*)
if ibv_devices | tail -n +3 | grep -q -E "rocep|mlx|qedr|rxe"; then
    DEVICE_NAME=$(ibv_devices | tail -n +3 | grep -v "^$" | head -1 | awk '{print $1}')
    echo "Found RDMA device: $DEVICE_NAME"
    ibv_devinfo -d $DEVICE_NAME || true
    echo ""
    echo "✓✓✓ SUCCESS: Driver loaded and RDMA device detected! ✓✓✓"
    exit 0
else
    echo "✗✗✗ FAILED: RDMA device not detected ✗✗✗"
    echo "ibv_devices output:"
    ibv_devices
    exit 1
fi
EOFVM

TEST_RESULT=$?

log_info ""
log_info "==================================================================="
if [ $TEST_RESULT -eq 0 ]; then
    log_info "✓✓✓ ALL TESTS PASSED ✓✓✓"
    log_info "==================================================================="
    log_info ""
    log_info "Driver successfully loaded and RDMA device detected in VM!"
    log_info ""
    log_info "You can SSH into the VM to continue testing:"
    log_info "  ssh -p 2222 stebates@localhost"
    log_info ""
    log_info "Press Ctrl+C to stop and cleanup"
    log_info ""
    
    # Keep running for manual testing
    log_info "Keeping VM and server running for manual testing..."
    wait $VM_PID
else
    log_error "✗✗✗ TESTS FAILED ✗✗✗"
    log_info "==================================================================="
    exit 1
fi

# Cleanup will happen via trap

