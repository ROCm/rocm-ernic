# ionic patches for transmitting from device memory, in their mainline form

The RFC "ionic: support netmem TX" as it would be posted: six patches and
a cover letter against the upstream Linux ionic Ethernet driver, with
which a netdev accepts a dma-buf for transmit (`NETDEV_CMD_BIND_TX`) and
sends from it with `sendmsg(MSG_ZEROCOPY)` and `SCM_DEVMEM_DMABUF`.

**For reference; nothing applies it.** It has not been posted upstream.
[`../devmem-tx`](../devmem-tx) is its backport to the kernel this
project's guests build ionic from, and is the series to give a guest.
This one is kept because [`../devmem-rx`](../devmem-rx) is based on it,
and because it is the form the tests on a `v7.4-rc3` kernel ran.

## Base

**`v7.4-rc3`** (`aebb72744764`, the `base-commit` in the cover letter),
with nothing below it.

That is not `IONIC_KERNEL_REF`, which
[`cmake/ErnicKernelModule.cmake`](../../../../cmake/ErnicKernelModule.cmake)
pins at `v7.2.6`, so **do not give this directory to
`fetch-ionic-sources.sh`**: `0004` patches
`Documentation/networking/device_drivers/ethernet/pensando/ionic.rst`,
which the sparse checkout does not have, so the series does not apply
there. No
role, variable, package or CI job names it. Like every `*.patch` under
this tree it is installed with the server
(`share/rocm-ernic/patches/devmem-tx-mainline/`), where nothing reads it.

| File | Touches | In `../devmem-tx` as |
| --- | --- | --- |
| `0000-cover-letter.patch` | — | — |
| `0001-ionic-dont-leave-a-failed-xdp-frame-in-the-tx-descriptor-info.patch` | `ionic_txrx.c` | `0002`, unchanged |
| `0002-ionic-give-tx-buffers-their-own-bookkeeping-struct.patch` | `ionic_dev.h`, `ionic_txrx.c` | `0003`, unchanged |
| `0003-ionic-unmap-tx-frags-with-the-netmem-helpers.patch` | `ionic_dev.h`, `ionic_txrx.c` | `0004`, unchanged |
| `0004-ionic-count-the-unreadable-skbs-dropped-on-tx.patch` | `ionic_lif.h`, `ionic_stats.c`, `ionic_txrx.c`, `ionic.rst` | `0005`, without the `ionic.rst` hunk |
| `0005-ionic-dont-send-an-unreadable-skb-that-has-no-frags.patch` | `ionic_txrx.c` | `0006`, unchanged |
| `0006-ionic-add-netmem-tx-support.patch` | `ionic_lif.c` | `0007`, other line numbers |

`../devmem-tx/0001` has no counterpart here: it is an upstream fix
`v7.4-rc3` already has.

`0005` is marked in the cover letter as not for net-next, which has
`23fe6ac2e97a` ("net: skbuff: make skb_copy_and_csum_bits() report
unreadable frags") and so no longer makes the skbs the patch drops.
`v7.4-rc3` does make them.

## The cover letter is behind the testing

`0000-cover-letter.patch` ends "Testing: build only ... Nothing has been
run". That was true when it was written and is not now; see below.
**Rewrite that paragraph before posting the series**, and re-read the
rest of the letter against the results: it was not updated after the
runs. The patches themselves are the files the runs used (the
`sha256sum` of each of `0001` to `0006` is what the test run recorded).

The patches carry no `Signed-off-by:`.

## What was tested

On `v7.4-rc3`:

- `git mailinfo` parses every file; each patch reproduces the tree the
  next was written against; `checkpatch.pl --strict` is clean but for
  the missing `Signed-off-by:`.
- The ionic directory builds after every patch with `W=1` and with
  sparse, gcc 13, and once more with the `dma_unmap_addr` macros compiled
  away.
- `ionic.ko`, built from the in-tree driver plus the six patches, was
  loaded in two guests of this emulator on a `v7.4-rc3` kernel with
  KASAN, lockdep, `PROVE_RCU`, `DEBUG_ATOMIC_SLEEP` and `DMA_API_DEBUG`,
  and ran the same matrix as the backport: a 360 s run moved 62.0 GiB
  with every byte verified at the receiver and all 66,522 zerocopy
  completions returned, none reporting a copy; unaligned offsets and
  lengths, up to 17 frags per GSO skb, MTU 1500 and 9000, TSO on and off,
  `XDP_TX` and `XDP_REDIRECT` on the sending netdev's one queue, and
  teardown with sends in flight passed, with no KASAN, lockdep or DMA-API
  unmap report from the driver.
  A second pass with a sender, receiver and probe written independently
  of the first re-ran most of the matrix (not the 360 s run, IPv6, MTU
  9198 or the XDP row) and confirmed each result it re-ran. It corrects
  one number in the commit messages and the cover letter: a sender is at
  risk of the 15-frag limit with pieces below about MSS/15 (about 97
  bytes at MTU 1500 and 600 at MTU 9000), not MSS/16.
- A dma-buf exported by amdgpu from GPU VRAM was bound and sent from on
  the same kernel built with `CONFIG_PCI_P2PDMA`; see
  [`docs/devmem.rst`](../../../../docs/devmem.rst).

Two things failed in those runs, neither in the series:

- Unloading `ionic` while a Tx binding is live ends in a use-after-free
  in the networking core when the binder closes its netlink socket.
  net-next has the fix, `ede1618064a9`; `v7.4-rc3` does not.
- Multi-buffer XDP (an `xdp.frags` program at MTU 9000) sent the wrong
  bytes in every second echoed frame; the devmem stream beside it still
  verified. That is
  the bug the base series' `0003` fixes
  ([`../README.md`](../README.md)); its mainline form is in
  [`tests/devmem/kernel-patches/ionic-mainline-fixes`](../../../../tests/devmem/kernel-patches/ionic-mainline-fixes).

**Not tested:** net-next (the patches other than `0005` apply to its
ionic files as of 2026-11-18, unbuilt), ionic hardware, `git am` of
these files onto a tree, and the kernel's own `devmem.py` selftest.

The reports are in
[`docs/devmem/notes`](../../../../docs/devmem/notes): `impl-driver.md`
for the series, `test-tx-7.4-rc3.md` and `test-tx-verification.md` for
the runs.

## Retiring this directory

Delete it when the series, or what replaces it, is upstream, or when
`../devmem-rx` no longer needs it as a base.
