<!--
Copyright (c) Advanced Micro Devices, Inc. All rights reserved.

SPDX-License-Identifier: MIT
-->

# The base ionic patch series

The patches in *this* directory are the ones every ERNIC guest gets. They
are applied to the ref `IONIC_KERNEL_REF` pins in
[`cmake/ErnicKernelModule.cmake`](../../../cmake/ErnicKernelModule.cmake),
currently `v7.2.6`, by `scripts/fetch-ionic-sources.sh`, whose
`--patches-dir` defaults to this directory. Every caller that builds the
driver a guest runs gets them, these without naming the directory at all:

| Caller | How it reaches this directory |
| --- | --- |
| `cmake/ErnicKernelModule.cmake` | `--patches-dir "${IONIC_PATCHES_DIR}"`, which is `ansible/files/patches` |
| `service/vm-driver-install.sh.in` | `--patches-dir "${SCRIPT_DIR}/patches"`; `ErnicInstall.cmake` installs every `*.patch` under this tree (not the READMEs) |
| `ernic_guest_setup` (`driver_ionic.yml`, `ionic_fetch_controller.yml`) | this directory first, then each of `ernic_guest_ionic_extra_patches` |
| `.github/workflows/ernic-driver-build.yml` | the `base` leg, and as `base_patches` under the `p4` and `devmem-tx` legs |

Two more callers name this directory explicitly, and so also get them:
`ernic_p4_dkms` (`ernic_p4_dkms_patches_dirs`, which must list both this
directory and `p4/`) and `ansible/playbooks/p4-tests.yml`, which copies
`patches/*.patch`.

The subdirectories are optional series applied **on top of** this one, each
with its own README: `srq/`, `p4/`, `devmem-tx/`, `devmem-tx-mainline/` and
`devmem-rx/` (both for reference only: they are against `v7.4-rc3`, and
nothing applies them) and `rdma-core/` (userspace, not the kernel).

## `--patches-dir` replaces, it does not add

A caller that passes `--patches-dir` loses this directory unless it names
it too. That is why `srq/` carries copies of all three patches below as its
`0009`, `0010` and `0011`: `ernic_srq_dkms` applies `patches/srq` alone, and
it builds `ionic.ko` as well as `ionic_rdma.ko`, so without the copies an
SRQ build would silently ship an eth driver missing fixes the normal build
has. **If you add or change a patch here, change the copy in `srq/` in the
same commit.** `srq/0010` and `srq/0011` are byte-identical to `0003` and
`0004`; `srq/0009` is `0002` regenerated, because the SRQ series shifts the
file it touches.

## What is here

| # | Subject | Why it is here | When to drop it |
| --- | --- | --- | --- |
| 0002 | ionic: allocate an address handle for UC queue pairs | without it every UC QP fails to reach RTR, so `ibv_uc_pingpong` cannot run | when the fix is in the pinned ref |
| 0003 | ionic: use the Rx buffer's page offset for XDP frags | an **information leak onto the wire**: XDP multi-buffer frags are built at page offset 0, so an `XDP_TX`/`XDP_REDIRECT` frame can carry another frame's bytes or stale kernel memory | when the fix is in the pinned ref |
| 0004 | ionic: free every temporary queue after a queue count change | a queue count change that grows the count leaks DMA allocations and leaves a page pool pointing at a freed NAPI, which a later page-pool dump reads (KASAN use-after-free) | when the fix is in the pinned ref |

### 0003 and 0004 are ours, and are not upstream

Both bugs are pre-existing: they are in mainline and in the pinned
`v7.2.6`, not in anything this repository adds. Both were found this week
by testing against the rocm-ernic userspace emulated NIC — 0003 with an
`xdp.frags` program echoing 8000-byte datagrams at MTU 9000, 0004 with
`ifdown; ethtool -L 2; ifup; ethtool -L 4`. Each patch's commit message
has the symptom, the root cause and the evidence.

**Both have been reported to the AMD Pensando team, who have not replied.**
They carry no `Signed-off-by:` — add yours when you post them.

**They must be dropped when the pinned ref moves to a kernel that has the
upstream fix.** Carrying a patch past the point where it is upstream is how
a tree ends up applying a fix twice, and here it would simply stop applying
and fail the build. The `ionic-patches` workflow checks that the patched
tree really contains both fixes, so a drop that is premature is caught
there rather than in a guest.

**Both have been run on the `v7.2.6` sources**, built as an external
module on a `v7.2.15` guest kernel, in two guests of this emulator. Two
modules were compared that differ in these two hunks only, `srq/0001` to
`0009` plus `p4`, and `srq/0001` to `0011` plus `p4`:

- Without 0003, 5000 of 10,000 echoed 8000-byte datagrams at MTU 9000
  had their last 426 bytes wrong, with `XDP_TX` and with `XDP_REDIRECT`,
  and one of the redirected ones carried stale kernel memory. With it, all 10,000 were intact with
  each.
- Without 0004, one `ifdown; ethtool -L 2; ifup; ethtool -L 4` left 12
  DMA allocations and two page pools behind, the unload reported
  `pending DMA allocations while released from device`, and a page-pool
  dump afterwards read through the orphan pools' stale NAPI pointers.
  With it, nine such sequences over three loads left nothing behind.
- The module with both passed TCP with the payload hashed at the
  receiver, `rping` and `ib_send_bw`, and 28 queue count, ring size, MTU
  and down/up changes under hash-verified TCP streams in both
  directions, with no DMA-API or other warning in the kernel log.

Not run there: KASAN (that kernel has none, so the use-after-free report
quoted in 0004 is from `v7.4-rc3`); the route a guest takes,
`fetch-ionic-sources.sh` and DKMS (the patches were applied with
`git apply` and built with `make M=`); the stock `v7.2.6` kernel; and the
base series without `srq/`. Under `devmem-tx`, and alone, both are
checked only by applying them to `v7.2.6` and building `ionic.ko` and
`ionic_rdma.ko` with `W=1` and no warnings. The hunks are identical on
`v7.2.6` and `v7.4-rc3`. The report is
[`test-ionic-fixes-7.2.6.md`](../../../docs/devmem/notes/test-ionic-fixes-7.2.6.md).
