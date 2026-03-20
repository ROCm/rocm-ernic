# rocm-ernic Tests

This directory contains test programs and infrastructure for testing
the rocm-ernic libvfio-user-based server.

## Test Programs

### test_pci_client

A simple vfio-user client that connects to the rocm-ernic server and
performs basic PCI configuration space queries.

**Tests Performed:**
- Socket connection to server
- PCI Vendor ID verification (AMD: 0x1022)
- PCI Device ID verification (ROCm ERNIC: 0x8000)
- PCI Class Code verification (Network Controller, Ethernet:
  0x02 00 00)
- PCI Header Type verification (Type 0)
- BAR register reads
- Interrupt configuration reads

**Exit Codes:**
- 0: All tests passed
- 1: Test failure or connection error

### test_data_transfer

Comprehensive RDMA data transfer test using libibverbs. Tests
send/recv operations and verifies data integrity and pattern
generation.

**Tests Performed:**
- RDMA device discovery and opening
- Protection Domain (PD) allocation
- Completion Queue (CQ) creation
- Queue Pair (QP) creation and state transitions
- Memory Region (MR) registration
- Basic send/recv operations
- Multiple transfers
- Varying buffer sizes (64 to 4096 bytes)

**Requirements:**
- libibverbs library
- RDMA device available (via rocm-ernic driver or hardware)

**Exit Codes:**
- 0: All tests passed
- 1: Test failure or no RDMA device available

**Note:** This test requires an RDMA device to be available. In CI,
this may require the rocm-ernic server running with loopback backend
and a VM with the driver loaded, or it will be skipped if no device
is found.

## Running Tests

### Quick Local Test

From project root:

```bash
./scripts/run-local-tests.sh
```

This will:
1. Check that binaries are built
1. Start the rocm-ernic server
1. Run the test client
1. Clean up automatically

### Manual Testing

Build the project:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Start the server in one terminal:

```bash
./build/rocm-ernic /tmp/test.sock
```

Run the test client in another terminal:

```bash
./build/tests/test_pci_client --socket /tmp/test.sock
```

### CTest

Run all tests via CTest:

```bash
ctest --test-dir build
```

With verbose output:

```bash
ctest --test-dir build --output-on-failure
```

## Test Infrastructure

### run-test.sh

Test harness script that orchestrates test execution:
- Starts server process in background
- Waits for server initialization
- Runs test client
- Captures server logs
- Cleans up processes and temporary files
- Reports test results

### CMakeLists.txt

CMake build configuration for tests:
- Builds test executables
- Registers tests with CTest
- Sets test timeouts (30 or 60 seconds)
- Ensures tests run sequentially (not in parallel)

## Adding New Tests

To add a new test:

1. Create test program in `tests/` directory
2. Add executable to `tests/CMakeLists.txt`
3. Register test with `add_test()`
4. Update this README

Example:

```cmake
add_executable(test_new_feature
    test_new_feature.c
)

add_test(
    NAME new-feature-test
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run-test.sh
        $<TARGET_FILE:test_new_feature>
        $<TARGET_FILE:rocm-ernic>
)
set_tests_properties(new-feature-test PROPERTIES
    TIMEOUT 30
    RUN_SERIAL TRUE
)
```
