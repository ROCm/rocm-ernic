project('vfu-pvrdma', 'c',
  version: '0.2.0',
  default_options: ['warning_level=1', 'c_std=gnu11'],
  license: 'GPL-2.0-or-later'
)

# Dependencies
vfiouser_dep = dependency('vfio-user', required: false)

# If vfio-user is not found via pkg-config, look in /usr/local
if not vfiouser_dep.found()
  vfiouser_dep = declare_dependency(
    include_directories: include_directories('/usr/local/include'),
    link_args: ['-L/usr/local/lib', '-lvfio-user']
  )
endif

# Additional dependencies
glib_dep = dependency('glib-2.0')
ibverbs_dep = dependency('libibverbs', required: true)

# RDMA backend sources from QEMU port
# Note: rdma.c excluded - only contains QEMU type system registration
rdma_backend_sources = files(
  'src/from-qemu/hw/rdma/rdma_backend.c',
  'src/from-qemu/hw/rdma/rdma_backend_core.c',    # Backend abstraction
  'src/from-qemu/hw/rdma/rdma_backend_none.c',    # "None" backend
  'src/from-qemu/hw/rdma/rdma_utils.c',
  'src/from-qemu/hw/rdma/rdma_rm.c',
)

# PVRDMA device sources from QEMU port
pvrdma_device_sources = files(
  'src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c',
  'src/from-qemu/hw/rdma/vmw/pvrdma_dev_ring.c',
  'src/from-qemu/hw/rdma/vmw/pvrdma_main.c',
  'src/from-qemu/hw/rdma/vmw/pvrdma_qp_ops.c',
)

# Utility sources
utility_sources = files(
  'src/from-qemu/utils/error-report.c'
)

# QEMU stub implementations
qemu_stub_sources = files(
  'src/from-qemu/qemu-stubs/qemu_stubs.c',
  'src/from-qemu/qemu-stubs/qemu_type_stubs.c'
)

# Compatibility bridge layer
bridge_sources = files(
  'src/vfu_compat_bridge.c'
)

# Main server implementation
server_sources = files(
  'src/vfu_pvrdma.c'
)

# Include directories
inc_dirs = include_directories(
  'src',
  'src/from-qemu/include/qemu-extra',
  '/usr/include/',
  '/usr/local/include',
)

# Compiler arguments
add_project_arguments(
  '-Wimplicit-function-declaration',
  '-D_GNU_SOURCE',
  '-DTARGET_PAGE_SIZE=4096',
  '-Wno-unused-parameter',  # QEMU headers have many unused parameters
  language: 'c'
)

# Build vfu_pvrdma executable with full PVRDMA backend
executable('vfu_pvrdma',
  [server_sources, bridge_sources, rdma_backend_sources, 
   pvrdma_device_sources, utility_sources, qemu_stub_sources],
  include_directories: inc_dirs,
  dependencies: [vfiouser_dep, glib_dep, ibverbs_dep],
  install: true,
  install_dir: get_option('bindir')
)

# Print summary
summary({
  'Version': meson.project_version(),
  'Build type': get_option('buildtype'),
  'C compiler': meson.get_compiler('c').get_id(),
  'Install prefix': get_option('prefix'),
}, section: 'Configuration')

summary({
  'libvfio-user': vfiouser_dep.found(),
  'glib-2.0': glib_dep.found(),
  'libibverbs': ibverbs_dep.found(),
}, section: 'Dependencies')

message('')
message('Building full PVRDMA device server with:')
message('  - libvfio-user integration')
message('  - QEMU PVRDMA device logic')
message('  - RDMA backend (libibverbs)')
message('  - Resource manager')
message('')

# Tests subdirectory
subdir('tests')
