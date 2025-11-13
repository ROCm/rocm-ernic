# Tests build configuration

# Test client programs
test_pci_client = executable(
    'test_pci_client',
    'test_pci_client.c',
    dependencies: [],
    install: false
)

test_bar1_read = executable(
    'test_bar1_read',
    'test_bar1_read.c',
    dependencies: [],
    install: false
)

test_dsr_trigger = executable(
    'test_dsr_trigger',
    'test_dsr_trigger.c',
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

# Register tests for meson test
test(
    'pci-config-test',
    find_program('run-test.sh'),
    args: [
        meson.current_build_dir() / 'test_pci_client',
        meson.project_build_root() / 'vfu_pvrdma'
    ],
    timeout: 30,
    is_parallel: false
)

