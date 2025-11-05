# Tests build configuration

# Test client program
test_pci_client = executable(
    'test_pci_client',
    'test_pci_client.c',
    dependencies: [],
    install: false
)

# Register test for meson test
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

