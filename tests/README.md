# rocm-ernic Tests

This directory contains test programs and infrastructure for testing
the rocm_ernic libvfio-user-based server.

## Test Programs

### test_pci_client

A simple vfio-user client that connects to the rocm_ernic server and
performs basic PCI configuration space queries.

**Tests Performed:**
- Socket connection to server
- PCI Vendor ID verification (AMD: 0x1022)
- PCI Device ID verification (ROCm ERNIC: 0x1484)
- PCI Class Code verification (Network Controller: 0x02xxxx)
- PCI Header Type verification (Type 0)
- BAR register reads
- Interrupt configuration reads

**Exit Codes:**
- 0: All tests passed
- 1: Test failure or connection error

### test_data_transfer

Comprehensive RDMA data transfer test using libibverbs. Tests send/recv
operations and verifies data integrity and pattern generation.

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
- RDMA device available (via rocm_ernic driver or hardware)

**Exit Codes:**
- 0: All tests passed
- 1: Test failure or no RDMA device available

**Note:** This test requires an RDMA device to be available. In CI, this
may require the rocm_ernic server running with loopback backend and a VM
with the driver loaded, or it will be skipped if no device is found.

## Running Tests

### Quick Local Test

From project root:

```bash
./scripts/run-local-tests.sh
```

This will:
1. Check that binaries are built
1. Start the rocm_ernic server
1. Run the test client
1. Clean up automatically

### Manual Testing

Build the project:

```bash
meson setup build
ninja -C build
```

Start the server in one terminal:

```bash
./build/rocm_ernic /tmp/test.sock
```

Run the test client in another terminal:

```bash
./build/tests/test_pci_client --socket /tmp/test.sock
```

### Meson Test Framework

Run all tests via meson:

```bash
meson test -C build
```

With verbose output:

```bash
meson test -C build --verbose
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

### meson.build

Meson build configuration for tests:
- Builds test client program
- Registers test with meson test framework
- Sets test timeout (30 seconds)
- Ensures tests run sequentially (not parallel)

## Adding New Tests

To add a new test:

1. Create test program in `tests/` directory
2. Add executable to `tests/meson.build`
3. Register test with `test()` function
4. Update this README

Example:

```meson
test_new_feature = executable(
    'test_new_feature',
    'test_new_feature.c',
    dependencies: [],
    install: false
)

test(
    'new-feature-test',
    find_program('run-test.sh'),
    args: [
        meson.current_build_dir() / 'test_new_feature',
        meson.project_build_root() / 'rocm_ernic'
    ],
    timeout: 30,
    is_parallel: false
)
```


