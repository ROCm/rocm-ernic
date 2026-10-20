# QEMU PVRDMA code

The code in [`qemu/`](qemu/README.md) is the core of QEMU's paravirtual RDMA
(PVRDMA) device model, originally from
[QEMU](https://gitlab.com/qemu-project/qemu)'s `hw/rdma/` and `hw/rdma/vmw/`.
The VMware device and ABI headers came to this tree through QEMU from the
Linux `vmw_pvrdma` driver. QEMU v9.0.0 is the upstream comparison point for
the current fork.

## Why QEMU removed it

QEMU deprecated PVRDMA and its RDMA subsystem in 8.2, then removed them in
9.1. The deprecation notice described the subsystem as being in bad shape
and without active maintenance. QEMU cited a security-critical patch that
had remained unmerged for more than six months, and said removal would go
ahead unless someone improved the code quality and added regression tests.
See the [deprecation commit](https://github.com/qemu/qemu/commit/e9a54265f533f4df957ee9ced3304df8c3ef2bea)
and [removal commit](https://github.com/qemu/qemu/commit/1dfd42c4264bbf47415a9e73f0d6b4e6a7cd7393).

## How far this fork has diverged

This is not a drop-in copy of upstream QEMU. It has been adapted to run as a
userspace device behind libvfio-user, with QEMU utility APIs supplied by
[`src/qemu-compat/`](../src/qemu-compat/) and RDMA operations dispatched to
rocm-ernic's backends. At the time of this README's review, the fork had
about 45 commits and differed from QEMU v9.0.0 by 9 to 415 changed lines per
file. It is no longer regularly kept in sync with QEMU, which does not
maintain the removed code.

## Plan: un-vendor and maintain it here

The model is part of rocm-ernic's guest-facing device boundary, not
third-party code that can be treated as read-only. The plan is to move it
out of `third-party/`, clean it up, and hold it to the same correctness,
security, testing, and maintenance standards as the rest of `src/`. Keep
the original copyright and license notices, and retain upstream
attribution.

Un-vendoring also needs build and tooling changes:

- Move the model's source and headers into the project's source tree and
  update the server and test source lists in `CMakeLists.txt` and
  `tests/CMakeLists.txt`.
- Remove the per-source `-w` warning suppression and stop treating the
  model's headers as SYSTEM includes.
- Include the files in clang-format, clang-tidy, and IWYU checks, fixing
  findings rather than relying on the current vendored-code exclusions.
- Keep the compatibility APIs needed by the standalone device, or replace
  them deliberately; do not pull in QEMU as a runtime dependency.
- Add or extend regression tests as behavior is hardened, especially for
  inputs controlled by the guest.

## Known risks to address

- [`rdma_rm.c`](qemu/hw/rdma/rdma_rm.c)'s `rdma_rm_alloc_uc()` has a
  commented-out PFN range check. Validate the guest-provided page frame
  against the device's BAR2 range before accepting or using it.
- [`qemu_stubs.c`](../src/qemu-compat/qemu_stubs.c) implements QEMU's
  `qatomic_*` helpers as plain, non-atomic operations. They are safe only
  under the current single-threaded device assumptions; do not start the
  vendored completion thread unless those operations and their shared state
  are made genuinely thread-safe.
- [`rdma_backend.c`](qemu/hw/rdma/rdma_backend.c) still has a TODO to query
  and retain the QP's `max_inline_data` capability for send operations.

Treat these as known work, not as an exhaustive audit. The guest controls
handles, addresses, lengths, and queue contents crossing this boundary;
validate them before use and test malformed inputs as the code is cleaned
up.
