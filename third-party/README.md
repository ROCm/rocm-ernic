# third-party

Everything here is in [`qemu/`](qemu/): the core of QEMU's paravirtual
RDMA (PVRDMA) device model, which rocm-ernic forked and has since
rewritten heavily. Most of what is in it now was written in this
repository, and nobody maintains the upstream original. It's ours to
change. The build treats it as vendored only so that its problems don't
all have to be fixed at once; once they are, it moves into `src/`. See
[Plan](#plan).

[`qemu/README.md`](qemu/README.md) covers the files themselves: what
each one is, how the build uses them, and their licenses.

## Where it came from

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
does that job.

### Origins

Three sources are mixed in these files:

- **QEMU**, by Oracle and Red Hat (Yuval Shaia and Marcel Apfelbaum),
  under `GPL-2.0-or-later`: the 11 files from `hw/rdma/`.
- **VMware**, under `(GPL-2.0 WITH Linux-syscall-note) OR
  BSD-2-Clause`: the three headers under `qemu/include/`, which VMware
  wrote for the Linux `vmw_pvrdma` driver and QEMU copied in with its
  kernel-header import.
- **rocm-ernic**: the changes described under
  [How far it is from upstream](#how-far-it-is-from-upstream), in nearly
  every file. The files still carry only the original copyright lines.

Nothing else was copied in. `rdma_backend_defs.h` replaces upstream's
include of QEMU's `contrib/rdmacm-mux/rdmacm-mux.h` with stubs of its
types, and its `struct ib_user_mad` is a hand-written stand-in with the
same field layout as the Linux uAPI structure of that name
(`include/uapi/rdma/ib_user_mad.h`), not a copy of it.

### Why upstream dropped it

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

## How far it is from upstream

Changed lines against QEMU v9.0.0, ignoring whitespace and blank lines:

| File                  | Upstream | Here | Changed |
|-----------------------|---------:|-----:|--------:|
| `rdma_rm.c`           |      812 | 1069 |     420 |
| `vmw/pvrdma_qp_ops.c` |      298 |  296 |     380 |
| `rdma_backend.c`      |     1401 | 1541 |     324 |
| `vmw/pvrdma.h`        |      144 |  129 |     139 |
| `rdma_backend_defs.h` |       76 |  183 |     123 |
| `rdma_rm_defs.h`      |      146 |  207 |      69 |
| The other 8 files     |          |      | 9 to 58 |

The changes are rocm-ernic features, not portability patches:

- A backend dispatch layer, `RdmaBackendOps`, with backend types for
  loopback, TCP, NVMe-oF, and S3.
- Dynamically connected (DC) queue pairs: `ROCM_ERNIC_PVRDMA_QPT_DCT`
  and `_DCI`, a DCT hash table, and `PVRDMA_WR_SEND_DC` and a `dc` work
  request member in `vmw_pvrdma-abi.h`.
- Per-QP and per-device statistics.
- Upstream's completion path is gone: `pvrdma_post_cqe()` no longer
  writes to a guest ring. It records the completion and sets a flag,
  and the main loop raises the interrupt, since libvfio-user isn't
  thread-safe.
- The QEMU dependencies are replaced with the shims in
  [`src/qemu-compat/`](../src/qemu-compat/).

About 45 commits in this repository changed these files, from the
first server commit in November 2025 until September 2026. They lived
in `src/from-qemu/` until PR #177 moved them here and labeled them
vendored. Since then, only commented-out code has been removed.

## Why it matters

The code is linked into the server and sits behind QP, CQ, MR, and PD
handles that the guest controls, so a bug in it is a guest-to-host bug.
No upstream will find or fix one: QEMU has deleted the code, and the
rocm-ernic changes exist only here.

## Plan

The code is going to be cleaned up, moved into `src/` alongside the code
that uses it, and held to the same rules as the rest of rocm-ernic's
source. It can be changed in the meantime; AGENTS.md asks that changes
to it follow the rules for `src/`.

Until it moves, the build still treats it as vendored, so it gets fewer
checks than `src/`:

- Its `.c` files compile with `-w`, and its headers are SYSTEM
  includes, so the compilers report nothing in it.
- clang-format, clang-tidy, and IWYU skip it.
- The coverage floor counts `src/` only.

CodeQL, the sanitizers, and valgrind already cover it, since they work
on the built program. `test_rdma_res_tbl` and
`test_rdma_backend_query_port` test parts of `rdma_rm.c` and
`rdma_backend.c` through their public functions, and
`ernic-test-ionic-harness` links all of it into the emulated device it
drives.

Moving it means taking it out of each of those exemptions and fixing
what they then report. Problems already known:

- `PVRDMADev.pending_cq_interrupt` is a `volatile int` set with the GNU
  builtin `__atomic_store_n()`, where AGENTS.md calls for
  `<stdatomic.h>`.
- The logging macros in `rdma_utils.h` use `##__VA_ARGS__`, another GNU
  extension.
- `rdma_backend.c` uses the `qatomic_*()` stubs, which aren't atomic
  (see "Known defects and traps" in [AGENTS.md](../AGENTS.md)).
- `PVRDMADev` still has `dhcp_server` and `dhcp_proxy` fields, which
  nothing uses since the DHCP code was removed.
- `rdma_backend.c` keeps upstream's `rdma_backend_init()` and the client
  side of the `rdmacm-mux` helper, and `rdma_backend_defs.h` stubs the
  helper's types. None of it is used: the server initializes the
  backend with `rdma_backend_init_with_ops()` in `src/rdma/`, and the
  character-device calls the helper's client makes are stubs in
  `src/qemu-compat/` that fail.
- CodeQL has open alerts in it, among them
  `cpp/integer-multiplication-cast-to-long` on the
  `g_malloc(tbl_sz * res_sz)` in `res_tbl_init()`.
