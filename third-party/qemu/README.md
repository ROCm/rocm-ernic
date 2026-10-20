# QEMU PVRDMA code

rocm-ernic's fork of the core of QEMU's paravirtual RDMA (PVRDMA)
device model (`https://gitlab.com/qemu-project/qemu`), which QEMU
removed in 9.1. It has been rewritten heavily here, can be changed like
the rest of rocm-ernic's source, and will move into `src/` once it's
cleaned up. [`../README.md`](../README.md) covers where it came from,
how far it is from upstream, and the plan for it.

## Files

- `hw/rdma/rdma_rm.c`, `.h`, and `rdma_rm_defs.h`: the resource
  manager, the tables of PDs, MRs, CQs, QPs, and SRQs, indexed by the
  handles the guest uses.
- `hw/rdma/rdma_backend.c`, `.h`, and `rdma_backend_defs.h`: the
  backend device, and the dispatch of SRQ operations to rocm-ernic's
  backends through `RdmaBackendOps`.
- `hw/rdma/rdma_utils.h`: logging macros.
- `hw/rdma/vmw/pvrdma.h`: the device state, `PVRDMADev`, and its
  statistics.
- `include/standard-headers/`: VMware's PVRDMA ABI header, from the
  Linux `vmw_pvrdma` driver by way of QEMU. Only its work request and
  completion layouts are used, to size the device's limits and to number
  the per-QP statistics.

## Build

- The server, `rocm-ernic`, builds the `.c` files listed in
  `ERNIC_VENDORED_SOURCES` in the top-level `CMakeLists.txt`. So does
  `ernic-test-ionic-harness`, and `test_rdma_res_tbl` builds
  `rdma_rm.c`, in `tests/CMakeLists.txt`.
- The sources include `rdma_backend_ops.h` from `src/rdma/` by bare
  name, so any target that builds them needs `src/rdma` on its include
  path.
- The QEMU APIs the code calls are implemented in
  [`src/qemu-compat/`](../../src/qemu-compat/).
- Until the code moves into `src/`, the build still treats it as vendored:
  the `.c` files compile with `-w`, both header trees are SYSTEM
  includes, and clang-format, clang-tidy, and IWYU skip it.

## Licenses

The QEMU sources are `GPL-2.0-or-later`. The VMware header under
`include/standard-headers/` is
`(GPL-2.0 WITH Linux-syscall-note) OR BSD-2-Clause`. Each file's SPDX
tag is authoritative; see also [`LICENSE.md`](../../LICENSE.md). Keep
each file's original copyright lines.
