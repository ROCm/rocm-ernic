project('libvfio-user-pvrdma', 'c')

glibc_dep = dependency('glib-2.0')

rdma_sources = [
  'hw/rdma/rdma.c',
  'hw/rdma/rdma_backend.c',
  'hw/rdma/rdma_utils.c',
  'hw/rdma/rdma_rm.c',
]

pvrdma_sources = [
  'hw/rdma/vmw/pvrdma_cmd.c',
  'hw/rdma/vmw/pvrdma_dev_ring.c',
  'hw/rdma/vmw/pvrdma_main.c',
  'hw/rdma/vmw/pvrdma_qp_ops.c',
]

other_sources = [
  'utils/error-report.c'
]

incdir = include_directories(
        ['./include/qemu-extra',
        '/usr/include/',
        '/home/stebates/Projects/qemu/build/',
        '/home/stebates/Projects/qemu/include',
        '/home/stebates/Projects/qemu/include/exec',
                '/home/stebates/Projects/qemu/include/hw/core',
        '/home/stebates/Projects/qemu/',
        '/usr/include'
        ])

add_project_arguments('-Wimplicit-function-declaration',
        language :'c')

executable('pvrdma',
  [ rdma_sources, pvrdma_sources, other_sources],
  include_directories : incdir,
  c_args: ['-D_GNU_SOURCE', '-DTARGET_PAGE_SIZE=4096'],
  dependencies :
  [glibc_dep]
)

