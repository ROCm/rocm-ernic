# rocm-ernic Tests

This directory contains test programs and infrastructure for testing
the rocm-ernic libvfio-user-based server.

## Test Programs

### test_pci_client

A simple vfio-user client that connects to the rocm-ernic server and
performs basic PCI configuration space queries.

**Tests Performed:**
- Socket connection to server
- PCI Vendor ID verification (Pensando: 0x1dd8)
- PCI Device ID verification (ROCm ERNIC: 0x1002)
- PCI subsystem verification (Pensando: 0x1dd8, ROCm ERNIC: 0x5400)
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

### test_ionic_ci.sh

Shell test for the emulated ionic device, registered with CTest as
`ionic-ci`. Needs neither a VM nor an RDMA device.

**Tests Performed:**
- Server starts on the `loopback` and `none` backends
- PCI identity verification (ionic ERNIC: `0x1dd8:0x1002`,
  subsystem `0x1dd8:0x5400`)
- BAR geometry (64K BAR0 with a 32K register window, 4M BAR2)
- MSI-X vector count (32)
- Clean shutdown on `SIGTERM`
- `--tap` attaches to an existing host TAP interface

**Requirements:**
- The TAP attach check is skipped unless `ERNIC_TEST_TAP` names an
  existing interface owned by the current user, since creating one
  needs `CAP_NET_ADMIN`:

  ```bash
  sudo ip tuntap add dev ernic-ci0 mode tap user "$USER"
  ERNIC_TEST_TAP=ernic-ci0 ctest --test-dir build -R ionic-ci
  ```

**Exit Codes:**
- 0: All tests passed
- 1: Test failure
- 77: Skipped (server binary not built)

### test_find_rdma_device.sh

Shell test registered with CTest as `find-rdma-device-tuple`. It uses a
synthetic sysfs tree to verify the shared RDMA-device lookup accepts the
emulated vendor/device/subsystem tuple and rejects devices that differ in
any tuple field.

### test_find_ipv4_gid_index.sh

Shell test registered with CTest as `find-ipv4-gid-index`. It uses
synthetic sysfs GID tables to verify `scripts/find-ipv4-gid-index.sh`
finds an address's IPv4-mapped GID at any index, picks the lowest of two,
lists the table when the GID is missing, and rejects malformed arguments.

### py/

Python suites that drive a real server over its vfio-user socket, the
way a VMM does, with no VM, RDMA hardware, root, or package outside the
standard library. Guest memory is a `memfd` that the client maps and
hands to the server, so the real DMA path runs.

`py/ernic/` holds the pieces: `vfu.py` (the vfio-user client and guest
memory), `server.py` (starting server processes and holding them to a
shutdown deadline), `ionic.py` (the ionic driver, the counterpart of
`ionic_harness.c`), `mesh.py` (the TCP mesh wire protocol and a fixture
that starts a manager and some workers), and `check.py` (the test
runner).

The suites, each registered with CTest:

- `py/test_vfu_client.py` (`ernic-vfu-client`): the vfio-user protocol
- `py/test_ionic_driver.py` (`ernic-ionic-driver`): the device, driven
  as the guest driver drives it
- `py/test_mesh_stress.py` (`ernic-mesh-stress`): a multi-node mesh
  under traffic, malformed peers, and churn

See `docs/testing.rst` for what each covers and how to run one directly.

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
2. Build it in `tests/CMakeLists.txt` with `ernic_add_test_program()`
   (`cmake/ErnicTesting.cmake`), which applies the project's flags;
   use a shared source list such as `ERNIC_TEST_ADMINQ_SOURCES` where
   one fits
3. Register a test that runs the program on its own with
   `ernic_add_unit_test()`, and one that needs a script around it with
   `add_test()`
4. Update this README

Examples:

```cmake
ernic_add_test_program(ernic-test-new-feature
    SOURCES
        test_new_feature.c
        ${ERNIC_TEST_ADMINQ_SOURCES}
    DEPS glib
    HEADER_DEPS vfio-user
)
ernic_add_unit_test(new-feature-unit ernic-test-new-feature TIMEOUT 30)

ernic_add_test_program(test_new_client SOURCES test_new_client.c)
add_test(
    NAME new-client-test
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run-test.sh
        $<TARGET_FILE:test_new_client>
        $<TARGET_FILE:rocm-ernic>
)
set_tests_properties(new-client-test PROPERTIES
    TIMEOUT 30
    RUN_SERIAL TRUE
)
```
