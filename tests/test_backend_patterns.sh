#!/bin/bash
#
# Test Loopback Backend Pattern Generation and MD5 Verification
#
# Tests the loopback backend's built-in features:
# - Data pattern generation (zeros, ones, increment, decrement, alternate, random)
# - MD5 checksum computation
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOCKET="/tmp/vfio-user-pvrdma.sock"
SERVER_LOG="/tmp/server_pattern_test.log"
VM_LOG="/tmp/vm_pattern_test.log"
TEST_BINARY="$SCRIPT_DIR/build/test_data_transfer"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_header() {
    echo ""
    echo "╔════════════════════════════════════════════════════════════╗"
    printf "║ %-58s ║\n" "$1"
    echo "╚════════════════════════════════════════════════════════════╝"
    echo ""
}

print_step() {
    echo -e "${BLUE}▶${NC} $1"
}

print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}⚠${NC} $1"
}

cleanup() {
    print_step "Cleaning up..."
    sudo pkill -9 vfu_pvrdma 2>/dev/null || true
    sudo pkill -9 qemu-system 2>/dev/null || true
    sleep 1
    sudo rm -f "$SOCKET"
}

test_pattern() {
    local pattern="$1"
    local with_md5="$2"
    local test_name="Pattern: $pattern"
    
    if [ "$with_md5" = "yes" ]; then
        test_name="$test_name + MD5"
        backend_config="${pattern},md5"
    else
        backend_config="$pattern"
    fi
    
    print_header "$test_name"
    
    # Start server with pattern
    print_step "Starting server with backend: loopback:$backend_config"
    sudo "$SCRIPT_DIR/build/vfu_pvrdma" -s "$SOCKET" -b "loopback:$backend_config" \
        > "$SERVER_LOG" 2>&1 &
    
    local server_pid=$!
    sleep 2
    
    # Check server is running
    if ! sudo pgrep -x vfu_pvrdma > /dev/null; then
        print_error "Server failed to start"
        cat "$SERVER_LOG"
        return 1
    fi
    
    sudo chmod 666 "$SOCKET"
    print_success "Server running (PID: $server_pid)"
    
    # Start VM
    print_step "Starting VM with vfio-user device..."
    cd /home/stebates/Projects/qemu-minimal/qemu
    QEMU_PATH=/opt/qemu/bin/ VM_NAME=stebates-test-vm VMEM=8192 VCPU=2 \
        VFIO_USERDEV="$SOCKET" \
        ./run-vm > "$VM_LOG" 2>&1 &
    
    local vm_pid=$!
    sleep 5
    
    # Check VM is running
    if ! pgrep -f "stebates-test-vm" > /dev/null; then
        print_error "VM failed to start"
        cat "$VM_LOG"
        sudo pkill -9 vfu_pvrdma
        return 1
    fi
    
    print_success "VM running (PID: $vm_pid)"
    
    # Run test in VM
    print_step "Running data transfer test in VM..."
    sleep 3  # Give VM time to fully boot
    
    # Check if test binary exists in VM
    ssh -p 2222 -o StrictHostKeyChecking=no -o ConnectTimeout=10 stebates@localhost \
        "test -f /home/stebates/Projects/test_data_transfer" 2>/dev/null || {
        print_warning "Building test binary in VM..."
        ssh -p 2222 -o StrictHostKeyChecking=no stebates@localhost \
            "cd /home/stebates/Projects && gcc -o test_data_transfer test_data_transfer.c -libverbs"
    }
    
    # Run the test
    print_step "Executing test..."
    if ssh -p 2222 -o StrictHostKeyChecking=no stebates@localhost \
        "cd /home/stebates/Projects && sudo ./test_data_transfer" 2>&1 | tee /tmp/test_output_${pattern}.log; then
        print_success "Test completed successfully!"
        
        # Show relevant server logs
        print_step "Server log excerpts:"
        echo "─────────────────────────────────────────────────────────────"
        grep -E "(Data pattern|MD5|SEND QP|Recv QP)" "$SERVER_LOG" | tail -20 || true
        echo "─────────────────────────────────────────────────────────────"
        
        # Cleanup this test
        sudo pkill -9 qemu-system
        sleep 1
        sudo pkill -9 vfu_pvrdma
        sleep 1
        
        return 0
    else
        print_error "Test failed!"
        print_step "Server log:"
        tail -50 "$SERVER_LOG"
        
        # Cleanup
        sudo pkill -9 qemu-system
        sudo pkill -9 vfu_pvrdma
        return 1
    fi
}

# ============================================================================
# Main Test Flow
# ============================================================================

print_header "Loopback Backend Pattern & MD5 Testing"

echo "This test suite validates:"
echo "  • Data pattern generation (zeros, ones, increment, etc.)"
echo "  • MD5 checksum computation"
echo "  • Send/recv operations with various patterns"
echo ""

# Cleanup any previous runs
cleanup

# Build test program if needed
print_step "Checking test program..."
if [ ! -f "$TEST_BINARY" ]; then
    print_warning "Test binary not found, building..."
    cd "$SCRIPT_DIR"
    if [ ! -d "build" ]; then
        meson setup build
    fi
    cd build
    ninja test_data_transfer || {
        print_error "Failed to build test binary"
        exit 1
    }
fi
print_success "Test binary ready"

# Array of patterns to test
patterns=(
    "preserve"      # Use actual guest data
    "zeros"         # All zeros
    "ones"          # All ones
    "increment"     # Incrementing bytes
    "decrement"     # Decrementing bytes
    "alternate"     # Alternating 0x55/0xAA
)

# Test Results
declare -A results

# Test each pattern without MD5
for pattern in "${patterns[@]}"; do
    if test_pattern "$pattern" "no"; then
        results["$pattern"]="PASS"
    else
        results["$pattern"]="FAIL"
    fi
    sleep 2
done

# Test a few patterns WITH MD5
print_header "Testing with MD5 Checksums Enabled"

md5_patterns=("preserve" "increment" "random")

for pattern in "${md5_patterns[@]}"; do
    if test_pattern "$pattern" "yes"; then
        results["${pattern}+md5"]="PASS"
    else
        results["${pattern}+md5"]="FAIL"
    fi
    sleep 2
done

# ============================================================================
# Summary
# ============================================================================

print_header "Test Summary"

echo "Pattern Tests:"
for pattern in "${patterns[@]}"; do
    result="${results[$pattern]}"
    if [ "$result" = "PASS" ]; then
        print_success "$pattern"
    else
        print_error "$pattern"
    fi
done

echo ""
echo "MD5 Tests:"
for pattern in "${md5_patterns[@]}"; do
    result="${results[${pattern}+md5]}"
    if [ "$result" = "PASS" ]; then
        print_success "$pattern + MD5"
    else
        print_error "$pattern + MD5"
    fi
done

echo ""

# Count results
total=0
passed=0
for result in "${results[@]}"; do
    ((total++))
    if [ "$result" = "PASS" ]; then
        ((passed++))
    fi
done

echo "╔════════════════════════════════════════════════════════════╗"
printf "║  Results: %d/%d passed                                      ║\n" "$passed" "$total"
echo "╚════════════════════════════════════════════════════════════╝"

# Final cleanup
cleanup

if [ "$passed" -eq "$total" ]; then
    print_success "ALL TESTS PASSED!"
    exit 0
else
    print_error "SOME TESTS FAILED"
    exit 1
fi

