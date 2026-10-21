# QEMU PVRDMA code

rocm-ernic's fork of the core of QEMU's paravirtual RDMA (PVRDMA)
device model (`https://gitlab.com/qemu-project/qemu`), which QEMU
removed in 9.1. It has been rewritten heavily here: most of what is in
it now was written in this repository, and nobody maintains the
upstream original. It's built and checked like the rest of `src/`, and
changed like it. [History](#history) covers where it came from and how
far it is from upstream.

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
  `QEMU_SOURCES` in the top-level `CMakeLists.txt`. So does
  `ernic-test-ionic-harness`, and `test_rdma_res_tbl` builds
  `rdma_rm.c`, in `tests/CMakeLists.txt`.
- The sources include their own headers, and `rdma_backend_ops.h` from
  `src/rdma/`, by bare name, so any target that builds them needs
  `src/qemu/hw/rdma`, `src/qemu/include`, and `src/rdma` on its include
  path.
- The QEMU APIs the code calls are implemented in
  [`src/qemu-compat/`](../qemu-compat/).
- `test_rdma_res_tbl` tests the resource manager through its public
  functions, and `ernic-test-ionic-harness` links all of it into the
  emulated device it drives.

## Licenses

The QEMU sources are `GPL-2.0-or-later`. The VMware header under
`include/standard-headers/` is
`(GPL-2.0 WITH Linux-syscall-note) OR BSD-2-Clause`. Each file's SPDX
tag is authoritative; see also [`LICENSE.md`](../../LICENSE.md). Keep
each file's original copyright lines.

## History

### Where it came from

QEMU carried the PVRDMA device in `hw/rdma/` from v2.12.0 (2018) to
v9.0.0. Upstream deprecated it in 8.2 (QEMU commit `e9a54265f5`) and
removed it in 9.1 (`1dfd42c426`), along with the `rdmacm-mux` helper.
Upstream, the files here changed after 8.0 only by losing two unused
macros, so v9.0.0, the last release with them, is the one to compare
against.

The fork took 11 of the 23 files in upstream's `hw/rdma/` and three
VMware headers from QEMU's `include/standard-headers/`. It left out the
device front end (`vmw/pvrdma_main.c`, `vmw/pvrdma_cmd.c`, and
`vmw/pvrdma_dev_ring.c` and `.h`), `rdma.c`, and the tracing and build
files. rocm-ernic's own front end in `src/`, the emulated ionic device,
does that job. Nine of the 14 files are left: `rdma_utils.c`,
`vmw/pvrdma_qp_ops.c` and `.h`, and two of the VMware headers were
deleted once nothing used them.

#### Origins

Three sources are mixed in these files:

- **QEMU**, by Oracle and Red Hat (Yuval Shaia and Marcel Apfelbaum),
  under `GPL-2.0-or-later`: the eight files from `hw/rdma/`.
- **VMware**, under `(GPL-2.0 WITH Linux-syscall-note) OR
  BSD-2-Clause`: the header under `include/`, which VMware wrote for
  the Linux `vmw_pvrdma` driver and QEMU copied in with its
  kernel-header import.
- **rocm-ernic**: the changes described under
  [How far it is from upstream](#how-far-it-is-from-upstream), in nearly
  every file. The files still carry only the original copyright lines.

Nothing else was copied in.

#### Why upstream dropped it

The deprecation says the subsystem was "in a bad shape" and that
nobody was picking up its patches. The evidence specific to the device:

- Its original maintainers had moved on. A fix for CVE-2023-1544 sat
  unmerged for more than six months, and a Coverity fix to
  `pvrdma_cmd.c` from Peter Maydell got no response from anyone working
  on the RDMA code for two months.
- Debian's QEMU maintainer had already turned it off
  (`--disable-pvrdma`), as "buggy and poorly maintained, resulting in
  numerous security issues", and said the remaining security patch
  couldn't be reviewed "because the code is rather scary".
- It had a run of CVEs in which the device trusted values from the
  guest: CVE-2021-3582 (an `mremap` with a length from the guest),
  CVE-2021-3607 (a ring with a page count of zero), CVE-2021-3608
  (unmapping DMA addresses that were never set), CVE-2022-1050 (device
  commands before the shared buffers exist), and CVE-2023-1544 (too many
  page-table entries). All five are in the front-end files the fork
  left out, but `src/` now does the same work, so the same mistakes
  could be made there.
- It had no tests.

Most of the harsh comments quoted about it, such as "This code should
never have passed review" and "I wouldn't let friends use it in
production", were about QEMU's RDMA live migration (`migration/rdma.c`),
not this device. The deprecation cited both together.

### How far it is from upstream

Changed lines against QEMU v9.0.0, ignoring whitespace and blank lines,
as of `26e1144` (#302). Later changes have moved the counts, not the
picture.

| File                  | Upstream | Here | Changed  |
|-----------------------|---------:|-----:|---------:|
| `rdma_backend.c`      |     1401 |   71 |     1374 |
| `rdma_rm.c`           |      812 |  829 |      714 |
| `vmw/pvrdma.h`        |      144 |   85 |      147 |
| `rdma_backend.h`      |      129 |   73 |      132 |
| `vmw_pvrdma-abi.h`    |      310 |  190 |      130 |
| `rdma_rm_defs.h`      |      146 |  109 |      101 |
| The other 3 files     |          |      | 46 to 66 |

The changes are rocm-ernic's, not portability patches:

- A backend dispatch layer, `RdmaBackendOps`, with backend types for
  loopback, TCP, NVMe-oF, and S3.
- Per-QP and per-device statistics.
- Everything the ionic front end doesn't use is gone: upstream's verbs
  backend and data path, its completion thread and completion path, MAD
  handling and the `rdmacm-mux` client, the GID table, user contexts,
  and the resource manager's own statistics. Most of the changed lines
  in `rdma_backend.c` and the headers are these deletions.
- The QEMU dependencies are replaced with the shims in
  [`src/qemu-compat/`](../qemu-compat/).

About 45 commits in this repository changed these files, from the
first server commit in November 2025 until September 2026. They lived
in `src/from-qemu/` until PR #177 moved them to `third-party/qemu/` and
labeled them vendored, exempt from the compiler warnings, clang-tidy,
IWYU, codespell, and the coverage floor until they were cleaned up.
Commented-out code, and then the code nothing used, were removed, and
the warnings the compilers then reported were fixed. Once the compilers,
clang-tidy, and codespell reported nothing in it, it moved here, with
the exemptions dropped.

### Why it matters

The code is linked into the server and sits behind QP, CQ, MR, and PD
handles that the guest controls, so a bug in it is a guest-to-host bug.
No upstream will find or fix one: QEMU has deleted the code, and the
rocm-ernic changes exist only here.
