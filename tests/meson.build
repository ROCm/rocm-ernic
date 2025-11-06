# Tests build configuration

# libvfio-user dependency (use local build)
libvfiouser_inc = include_directories('/home/stebates/Projects/libvfio-user/include')
libvfiouser_link = ['-L/home/stebates/Projects/libvfio-user/build/lib', '-lvfio-user']

# Test client programs
test_pci_client = executable(
    'test_pci_client',
    'test_pci_client.c',
    dependencies: [],
    install: false
)

test_pvrdma_device = executable(
    'test_pvrdma_device',
    'test_pvrdma_device.c',
    include_directories: [libvfiouser_inc],
    link_args: libvfiouser_link,
    install: false
)

test_bar1_read = executable(
    'test_bar1_read',
    'test_bar1_read.c',
    dependencies: [],
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

test(
    'pvrdma-device-test',
    find_program('run-test.sh'),
    args: [
        meson.current_build_dir() / 'test_pvrdma_device',
        meson.project_build_root() / 'vfu_pvrdma'
    ],
    timeout: 30,
    is_parallel: false
)

