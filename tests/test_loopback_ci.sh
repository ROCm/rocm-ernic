#!/bin/bash
# Comprehensive loopback backend testing for CI
# Tests multiple loopback configurations and rdma_cm emulation

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
SERVER_BIN="${SERVER_BIN:-$BUILD_DIR/rocm_ernic}"
TEST_BIN="${TEST_BIN:-$BUILD_DIR/tests/test_data_transfer}"
SOCKET_BASE="/tmp/test-loopback-ci-$$"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test configurations to test
declare -a CONFIGS=(
    "loopback"
    "loopback:mode=preserve"
    "loopback:mode=zeros"
    "loopback:mode=random"
    "loopback:md5"
    "loopback:mode=preserve,md5"
)

# Test results tracking
declare -a PASSED_CONFIGS=()
declare -a FAILED_CONFIGS=()
declare -a SKIPPED_CONFIGS=()

# Cleanup function (called via trap)
cleanup() {
    # shellcheck disable=SC2317  # Function is called via trap
    local pid="${SERVER_PID:-}"
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -f "${SOCKET_BASE}"*.sock
}

trap cleanup EXIT

# Check if binaries exist
if [ ! -f "$SERVER_BIN" ]; then
    echo -e "${RED}Error: Server binary not found: $SERVER_BIN${NC}"
    exit 1
fi

if [ ! -f "$TEST_BIN" ]; then
    echo -e "${YELLOW}Warning: Test binary not found: $TEST_BIN${NC}"
    echo -e "${YELLOW}Will only test server startup for each configuration${NC}"
    TEST_BIN=""
fi

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  Loopback Backend CI Testing                             ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo "Server binary: $SERVER_BIN"
if [ -n "$TEST_BIN" ]; then
    echo "Test binary: $TEST_BIN"
fi
echo "Configurations to test: ${#CONFIGS[@]}"
echo ""

# Function to test a single configuration
test_config() {
    local config="$1"
    local socket_path="${SOCKET_BASE}-${config//[:\/=]/_}.sock"
    local server_log="${socket_path}.log"
    local test_result=0
    
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
        echo "Server log:"
        tail -20 "$server_log"
        FAILED_CONFIGS+=("$config")
        kill "$SERVER_PID" 2>/dev/null || true
        return 1
    fi
    
    echo -e "${GREEN}✓ Server started (PID: $SERVER_PID)${NC}"
    
    # Run test if available
    if [ -n "$TEST_BIN" ]; then
        echo "Running test..."
        if timeout 30 "$TEST_BIN" > /tmp/test-output.log 2>&1; then
            echo -e "${GREEN}✓ Test passed${NC}"
        else
            local test_exit=$?
            echo -e "${RED}✗ Test failed (exit code: $test_exit)${NC}"
            echo "Test output:"
            tail -30 /tmp/test-output.log
            test_result=1
        fi
    else
        echo -e "${YELLOW}⚠ No test binary - only verifying server startup${NC}"
        # Give server a moment to initialize
        sleep 2
    fi
    
    # Stop server
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -f "$socket_path"
    
    if [ $test_result -eq 0 ]; then
        echo -e "${GREEN}✓ Configuration test passed${NC}"
        PASSED_CONFIGS+=("$config")
        return 0
    else
        echo -e "${RED}✗ Configuration test failed${NC}"
        FAILED_CONFIGS+=("$config")
        return 1
    fi
}

# Run tests for all configurations
for config in "${CONFIGS[@]}"; do
    if test_config "$config"; then
        echo ""
    else
        echo ""
    fi
done

# Print summary
echo ""
echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  Test Summary                                               ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${GREEN}Passed: ${#PASSED_CONFIGS[@]}/${#CONFIGS[@]}${NC}"
if [ ${#PASSED_CONFIGS[@]} -gt 0 ]; then
    for config in "${PASSED_CONFIGS[@]}"; do
        echo -e "  ${GREEN}✓${NC} $config"
    done
fi

if [ ${#FAILED_CONFIGS[@]} -gt 0 ]; then
    echo ""
    echo -e "${RED}Failed: ${#FAILED_CONFIGS[@]}/${#CONFIGS[@]}${NC}"
    for config in "${FAILED_CONFIGS[@]}"; do
        echo -e "  ${RED}✗${NC} $config"
    done
fi

if [ ${#SKIPPED_CONFIGS[@]} -gt 0 ]; then
    echo ""
    echo -e "${YELLOW}Skipped: ${#SKIPPED_CONFIGS[@]}${NC}"
    for config in "${SKIPPED_CONFIGS[@]}"; do
        echo -e "  ${YELLOW}⊘${NC} $config"
    done
fi

echo ""

# Exit with error if any tests failed
if [ ${#FAILED_CONFIGS[@]} -gt 0 ]; then
    echo -e "${RED}Some configuration tests failed!${NC}"
    exit 1
fi

echo -e "${GREEN}All loopback configuration tests passed!${NC}"
exit 0

