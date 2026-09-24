# Vendored QEMU code

The PVRDMA device model from QEMU's `hw/rdma/`, plus three VMware uAPI
headers from QEMU's `include/standard-headers/`
(`https://gitlab.com/qemu-project/qemu`). The files carry local changes
made in this repository and are not kept in step with upstream QEMU.

This code is not ours to fix. Do not edit these files; work around a
problem from the code under `src/` or from the build configuration.

- The `.c` files are compiled with `-w`, and both header trees are SYSTEM
  includes, so their warnings do not surface in our code.
- The QEMU APIs this code calls are implemented in `src/qemu-compat/`.
- It includes `rdma_backend_ops.h` and `pvrdma_comp_ctx.h` from
  `src/rdma/` by bare name, so any target that builds these sources needs
  `src/rdma` on its include path.

The QEMU sources are `GPL-2.0-or-later`; the VMware headers under
`include/standard-headers/` are dual `GPL-2.0` / `BSD-2-Clause`. Each
file's SPDX header is authoritative.
