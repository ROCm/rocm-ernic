<!--
Copyright (C) Advanced Micro Devices, Inc.
SPDX-License-Identifier: GPL-2.0-or-later
-->

# net core: peer-to-peer dma-buf attach for devmem bindings (v7.2.x)

One patch for the Linux networking **core**, not the ionic driver:

    0001-net-devmem-allow-peer-to-peer-dma-buf-attach-for-bind.patch

**This is an upstream commit backported so that a test kernel could be
built.** It is not a fix of ours and not a change to this project:

    69ab87acc614 ("net: devmem: allow peer-to-peer dma-buf attach for
    bindings")
    Erfan Sharafzadeh, first in v7.4-rc1

**This repository ships no kernels.** Nothing here applies this patch,
no role or CMake target can, and the guest images the tests run on do
not have it: a kernel carrying it has to be built and installed by hand.
See "What an image bake would need" for what carrying it properly would
cost. The patch has no `Signed-off-by:` line, so upstream's own
`Signed-off-by:`, `Acked-by:` and `Link:` trailers are not carried
either; the author and the ack are named in the patch's bracketed note.

## What it is for

A devmem TCP binding — `NETDEV_CMD_BIND_TX` or `NETDEV_CMD_BIND_RX` —
attaches the dma-buf to the NIC. On `v7.2.x` that attach is a plain
`dma_buf_attach()`, so `attach->peer2peer` stays false, and amdgpu's
`amdgpu_dma_buf_pin()` then strips `AMDGPU_GEM_DOMAIN_VRAM` from the pin
domains. The buffer is moved to GTT — system memory — and the NIC is
handed system-RAM addresses. The transfer is still correct; it is simply
not device memory any more. Binding a `hipMalloc` buffer that way also
evicts and restores the KFD process, after which HIP's next
device-to-host copy of the buffer returns nothing or hangs.

With this commit the binding attaches dynamically with
`allow_peer2peer`, amdgpu pins the buffer where it is, and the NIC is
given GPU BAR addresses. **One commit covers both directions**: no
Rx-specific core change was needed.

It does **not** change `--rocm-rdma-mode vram` (`ibv_reg_mr()` of a
`hipMalloc` address), which fails in `pin_user_pages_fast()` because the
mapping is `VM_IO | VM_PFNMAP` — a path this patch does not touch.

## Where it applies

The upstream patch does not apply to `v7.2.y`: three of its six hunks
fail on context, because `v7.2.y` still allocates net_iovs from a
genpool. This is a hand-backport that moves the same lines to where
`v7.2.y` has the corresponding code; the added and removed code lines
are identical to upstream's. The patch's own note lists the four places
that moved.

`net/core/devmem.c` is byte-identical in `v7.2.6` and `v7.2.15`, so
**one file serves both**. `git apply --check` passes on each.

It needs nothing else from `v7.3`/`v7.4`: `dma_buf_dynamic_attach()`,
`dma_buf_pin()`, `dma_buf_unpin()`, `dma_resv_wait_timeout()` and the
optional `invalidate_mappings` importer op are all in `v7.2`.

## What the kernel it was built into also needs

| Need | Why |
| --- | --- |
| `CONFIG_PCI_P2PDMA=y`, and `ZONE_DEVICE`, `NET_DEVMEM`, `DMA_SHARED_BUFFER`; `HSA_AMD_P2P` for KFD buffers | amdgpu registers VRAM as P2P memory and its dma-buf attach asks `pci_p2pdma_distance()`. All are `=y` in the test guests' stock config |
| A guest CPU that is `AuthenticAMD`, family 0x17 or later | the emulated NIC and GPU sit under different root ports of a host bridge that is not on the p2pdma whitelist, so `cpu_supports_p2pdma()` is what accepts the pair. The guests run `-cpu EPYC`, which needs an AMD Zen host. On another host or CPU model the distance check fails and amdgpu falls back to GTT **silently** |
| The fork QEMU and the matching NIC server | only the fork hands the NIC server the GPU's BAR, without which the server cannot reach a BAR address |
| A NIC driver with devmem Tx, and for receive devmem Rx as well | otherwise the bind is refused. See [`ansible/files/patches/devmem-tx`](../../../../ansible/files/patches/devmem-tx) and [`devmem-rx-7.2.6`](../../../../ansible/files/patches/devmem-rx-7.2.6) |
| amdgpu-dkms 31.70 (7.2.3), unchanged | its `amdgpu_dma_buf_pin()` already allows VRAM for a `peer2peer` attachment |

## What was checked

- `git mailinfo` parses the file; `git apply --check --index` passes on
  pristine `v7.2.6` and `v7.2.15`; the added and removed code lines are
  identical to upstream's, compared sorted; `checkpatch.pl
  --no-signoff`: 0 errors. **`git am` was not run.**
- A `v7.2.6` kernel was built from it with the guest's own full config
  (four lines differ from stock: two key-file paths and the compiler and
  pahole version strings), installed in **both** test guests, and every
  out-of-tree module rebuilt for it.
- **Both guests ran it**, through the whole upstream quick-start set,
  the SRQ tests, a 35-case devmem Tx matrix, devmem Tx from GPU VRAM up
  to 1020 MiB, the devmem Rx bring-up and the GPU-to-GPU end-to-end
  runs. Every quick-start verdict equals the stock kernel's; no splat in
  any boot. A `hipMalloc` buffer stayed in VRAM through 67 cases with
  KFD `evicted_ms` 0, where the stock kernel evicted it.
- A lockdep build of the same kernel reported nothing over one boot in
  one guest.

**Not run:** a `v7.2.15` kernel with it (the patch applies; it was not
built); an image bake; the upstream commit's own named limit, a second
non-peer-to-peer attachment to a buffer a peer-to-peer binding has
pinned in VRAM; `amdgpu_evict_vram` on this kernel; kmemleak, which the
guest kernels do not have.

## What an image bake would need

The guest image takes its kernel from a mainline PPA, which cannot carry
a patch. The role that does it, `kernel.yml`, is in qemu-minimal, not in
this repository. As read there, and **not tried**:

1. **A custom kernel package.** `kernel.yml` can already take debs from
   a controller directory instead of the PPA, but it **asserts a
   `linux-modules` deb, which `bindeb-pkg` does not produce** — that
   packaging puts the modules inside `linux-image`. Either relax the
   assert, or build with the PPA's own packaging plus this patch, which
   would also keep the stock release name.
2. **A release string that moves.** The test kernel is
   `7.2.6-devmemp2p`. The grub pin and the boot assert accept it (they
   match `vmlinuz-7.2.6-*` and compare only the part before the first
   `-`), and the image stamps record whatever is running, but anything
   that spells the stock release literally has to change.
3. **The DKMS and out-of-tree modules all rebuild unchanged**, in the
   bake's normal order: amdgpu-dkms by autoinstall from the kernel
   package's postinst, then ionic-ernic and ernic-p4 by an explicit
   `dkms install -k` (both are `AUTOINSTALL=no`), then `sdxi_char` and
   `sdxi_perf`, which are not DKMS and whose sources are not in the
   image. No source change in any of them.
4. Module signing: a build like the test one generates a throwaway key
   that is enrolled nowhere, so DKMS modules taint the kernel exactly as
   on the stock image. `CONFIG_MODULE_SIG_FORCE` is not set.

## When to drop it

When the guest kernel is `v7.4` or later, which has `69ab87acc614`.
Remove it then; it will not apply.

## The other net-core patch here

[`../net-core-v7.2.15`](../net-core-v7.2.15) holds an unrelated core
backport, the fix for a use-after-free when a netdev is unregistered
with a devmem Tx binding live. The two are kept apart because they have
different bases, different reasons and the same `0001-` number; neither
is renumbered, because each was verified under the name it has.
