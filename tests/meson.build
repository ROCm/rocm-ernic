# Tests build configuration

# Test client programs
test_pci_client = executable(
    'test_pci_client',
    'test_pci_client.c',
    dependencies: [],
    install: false
)

# Data transfer test (requires libibverbs)
test_data_transfer = executable(
    'test_data_transfer',
    'test_data_transfer.c',
    dependencies: [ibverbs_dep],
    install: false
)

# RDMA Connection Manager (rdma_cm) emulation test
test_rdma_cm = executable(
    'test_rdma_cm',
    'test_rdma_cm.c',
    dependencies: [ibverbs_dep],
    install: false
)

# Register tests for meson test
test(
    'pci-config-test',
    find_program('run-test.sh'),
    args: [
        meson.current_build_dir() / 'test_pci_client',
        meson.project_build_root() / 'rocm_ernic'
    ],
    timeout: 30,
    is_parallel: false
)

# Data transfer test (requires RDMA device - may be skipped if none available)
test(
    'data-transfer-test',
    test_data_transfer,
    timeout: 60,
    is_parallel: false
)

# RDMA Connection Manager (rdma_cm) emulation test
# Tests connection info query when QPs are auto-paired
test(
    'rdma-cm-test',
    test_rdma_cm,
    timeout: 60,
    is_parallel: false
)

# Loopback configuration tests (requires server running)
# Tests multiple backend configurations
# Note: This test requires the server binary and test_rdma_cm binary
# It's run manually or via CI, not automatically by meson test

