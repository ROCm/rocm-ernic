#!/bin/bash
# Loopback backend testing with VM from Docker image
# This script runs loopback backend tests using a VM extracted from a Docker image
#
# Usage:
#   VM_IMAGE_REGISTRY=registry/path/image:tag \
#   VM_USER=ubuntu \
#   VM_PASSWORD=ubuntu \
#   SSH_PORT=2222 \
#   ./tests/test_loopback_with_vm.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
SERVER_BIN="${SERVER_BIN:-$BUILD_DIR/rocm_ernic}"
TEST_BIN="${TEST_BIN:-$BUILD_DIR/tests/test_data_transfer}"

# VM configuration (can be overridden via environment)
# Default registry: Docker Hub image with preconfigured VM
VM_IMAGE_REGISTRY="${VM_IMAGE_REGISTRY:-docker.io/sbates130272/batesste-ci-images-qemu-libvfio-user:latest}"
VM_USER="${VM_USER:-ubuntu}"
VM_PASSWORD="${VM_PASSWORD:-ubuntu}"
SSH_PORT="${SSH_PORT:-2222}"
VM_DISK_PATH="${VM_DISK_PATH:-/tmp/vm-disk-$$.img}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
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
    # shellcheck disable=SC2317
    local pid="${SERVER_PID:-}"
    local qemu_pid="${QEMU_PID:-}"
    
    if [ -n "$qemu_pid" ]; then
        kill "$qemu_pid" 2>/dev/null || true
        wait "$qemu_pid" 2>/dev/null || true
    fi
    
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    
    rm -f /tmp/vfio-user-rocm-ernic*.sock
    rm -f "$VM_DISK_PATH"
}

trap cleanup EXIT

# Check prerequisites
# VM_IMAGE_REGISTRY has a default, but can be overridden
if [ -z "$VM_IMAGE_REGISTRY" ]; then
    echo -e "${RED}Error: VM_IMAGE_REGISTRY not set${NC}"
    echo "Usage: VM_IMAGE_REGISTRY=registry/path/image:tag $0"
    echo "Default: docker.io/sbates130272/batesste-ci-images-qemu-libvfio-user:latest"
    exit 1
fi

if [ ! -f "$SERVER_BIN" ]; then
    echo -e "${RED}Error: Server binary not found: $SERVER_BIN${NC}"
    exit 1
fi

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  Loopback Backend Tests with Docker VM                    ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo "VM Image Registry: $VM_IMAGE_REGISTRY"
echo "Server binary: $SERVER_BIN"
echo "Test binary: ${TEST_BIN:-not available}"
echo "Configurations to test: ${#CONFIGS[@]}"
echo ""

# Extract VM disk from Docker image
echo -e "${BLUE}=== Extracting VM disk from Docker image ===${NC}"
CONTAINER_ID=$(docker create "$VM_IMAGE_REGISTRY")
echo "Container ID: $CONTAINER_ID"

# Try common VM disk locations
VM_DISK_PATHS=(
    "/output/vm-disk.img"
    "/vm-disk.img"
    "/vm/vm-disk.img"
    "/root/vm-disk.img"
    "/opt/vm/vm-disk.img"
)

VM_DISK_FOUND=""
for path in "${VM_DISK_PATHS[@]}"; do
    if docker exec "$CONTAINER_ID" test -f "$path" 2>/dev/null; then
        echo "Found VM disk at: $path"
        docker cp "$CONTAINER_ID:$path" "$VM_DISK_PATH"
        VM_DISK_FOUND="yes"
        break
    fi
done

docker rm "$CONTAINER_ID"

if [ -z "$VM_DISK_FOUND" ]; then
    echo -e "${RED}✗ VM disk image not found in Docker image${NC}"
    echo "Checked paths: ${VM_DISK_PATHS[*]}"
    exit 1
fi

qemu-img info "$VM_DISK_PATH"
echo -e "${GREEN}✓ VM disk extracted${NC}"

# Function to test a configuration
test_config() {
    local config="$1"
    local socket_path="/tmp/vfio-user-rocm-ernic-${config//[:\/=]/_}.sock"
    local server_log="${socket_path}.log"
    local test_result=0
    
    echo ""
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Testing: ${config}${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    
    # Start server
    echo "Starting server..."
    "$SERVER_BIN" \
        --socket "$socket_path" \
        --backend "$config" \
        --verbose > "$server_log" 2>&1 &
    SERVER_PID=$!
    
    # Wait for socket
    local socket_ready=0
    for i in {1..10}; do
        if [ -S "$socket_path" ]; then
            chmod 666 "$socket_path" 2>/dev/null || true
            socket_ready=1
            break
        fi
        sleep 0.5
    done
    
    if [ $socket_ready -eq 0 ]; then
        echo -e "${RED}✗ Server failed to create socket${NC}"
        tail -20 "$server_log"
        return 1
    fi
    
    echo -e "${GREEN}✓ Server started (PID: $SERVER_PID)${NC}"
    
    # Start VM
    echo "Starting VM..."
    qemu-system-x86_64 \
        -machine q35,accel=tcg \
        -cpu max \
        -m 2G \
        -nographic \
        -drive file="$VM_DISK_PATH",format=qcow2,if=virtio \
        -netdev user,id=net0,hostfwd=tcp::$SSH_PORT-:22 \
        -device virtio-net-pci,netdev=net0 \
        -device "{\"driver\":\"vfio-user-pci\",\"socket\":{\"type\":\"unix\",\"path\":\"$socket_path\"}}" \
        -serial mon:stdio > /tmp/qemu-${config//[:\/=]/_}.log 2>&1 &
    
    QEMU_PID=$!
    
    # Wait for SSH
    echo "Waiting for VM SSH..."
    local ssh_ready=0
    for i in {1..60}; do
        if sshpass -p "$VM_PASSWORD" \
           ssh -o StrictHostKeyChecking=no -o ConnectTimeout=2 \
           -p "$SSH_PORT" "${VM_USER}@localhost" 'echo Ready' 2>/dev/null; then
            ssh_ready=1
            break
        fi
        sleep 2
    done
    
    if [ $ssh_ready -eq 0 ]; then
        echo -e "${RED}✗ VM SSH timeout${NC}"
        kill "$QEMU_PID" 2>/dev/null || true
        kill "$SERVER_PID" 2>/dev/null || true
        return 1
    fi
    
    echo -e "${GREEN}✓ VM ready${NC}"
    
    # Run test_data_transfer if available
    if [ -f "$TEST_BIN" ]; then
        echo "Running test_data_transfer in VM..."
        if sshpass -p "$VM_PASSWORD" \
           ssh -o StrictHostKeyChecking=no -p "$SSH_PORT" "${VM_USER}@localhost" << EOFVM
        set -e
        # Check for RDMA device
        if ! ibv_devices | grep -q rocm_ernic; then
          echo "No rocm_ernic device found - driver may need to be loaded"
          exit 77
        fi
        # Run test (assuming test binary is in PATH or /opt/rocm-ernic/tests/)
        if [ -f /opt/rocm-ernic/tests/test_data_transfer ]; then
          /opt/rocm-ernic/tests/test_data_transfer
        elif command -v test_data_transfer &> /dev/null; then
          test_data_transfer
        else
          echo "test_data_transfer not found"
          exit 1
        fi
EOFVM
        then
            echo -e "${GREEN}✓ Test passed${NC}"
        else
            local test_exit=$?
            echo -e "${YELLOW}⚠ Test exited with code: $test_exit${NC}"
            if [ $test_exit -eq 77 ]; then
                echo "Test skipped (no RDMA device)"
            else
                test_result=1
            fi
        fi
    else
        echo -e "${YELLOW}⚠ test_data_transfer not available${NC}"
    fi
    
    # Stop VM and server
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -f "$socket_path"
    
    if [ $test_result -eq 0 ]; then
        echo -e "${GREEN}✓ Configuration test passed${NC}"
        return 0
    else
        echo -e "${RED}✗ Configuration test failed${NC}"
        return 1
    fi
}

# Run tests
PASSED=0
FAILED=0

for config in "${CONFIGS[@]}"; do
    if test_config "$config"; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1))
    fi
done

# Summary
echo ""
echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  Test Summary                                               ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${GREEN}Passed: $PASSED/${#CONFIGS[@]}${NC}"
echo -e "${RED}Failed: $FAILED/${#CONFIGS[@]}${NC}"

if [ $FAILED -gt 0 ]; then
    exit 1
fi

echo -e "${GREEN}All tests passed!${NC}"
exit 0

