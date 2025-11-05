project('vfu-pvrdma', 'c',
  version: '0.1.0',
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

# Main server implementation (standalone, no QEMU dependencies yet)
vfu_server_sources = files(
  'src/vfu_pvrdma.c'
)

# Build vfu_pvrdma executable (libvfio-user server)
# NOTE: This is a minimal implementation for testing
# RDMA backend integration from QEMU code will be added in future commits
executable('vfu_pvrdma',
  vfu_server_sources,
  dependencies: [vfiouser_dep],
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
}, section: 'Dependencies')

message('')
message('NOTE: This builds a minimal PVRDMA device server for testing.')
message('      RDMA backend (from QEMU) integration is not yet included.')
message('      The device will enumerate correctly but RDMA operations are stubbed.')
message('')
