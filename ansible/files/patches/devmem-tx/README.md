# ionic patches for transmitting from device memory (devmem TCP)

Eight patches against the upstream Linux ionic Ethernet driver that let a
guest bind a dma-buf to its ionic netdev for transmit
(`NETDEV_CMD_BIND_TX`) and send from it with `sendmsg(MSG_ZEROCOPY)` and
`SCM_DEVMEM_DMABUF`.

**Experimental.** The series is a backport of an RFC that has not been
posted upstream. `0001-0007` were built, with `W=1` and no warnings,
against a v7.2.15 kernel and run between two guests of this emulator;
`0008` came later and was built and run on v7.2.6 guests. No ionic
hardware has run any of it. See "What was tested" below for what was
not.

## Base

Applies to **`v7.2.6`** of
`https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git`, the
ref `IONIC_KERNEL_REF` pins in
[`cmake/ErnicKernelModule.cmake`](../../../../cmake/ErnicKernelModule.cmake),
with or without the base series in [`../`](../), [`../srq`](../srq) and
[`../p4`](../p4) below it: none of those touches the lines it changes.

Give the directory **whole**, as it is. The devmem Rx series in
[`../devmem-rx-7.2.6`](../devmem-rx-7.2.6) goes on top of all eight of
these, and is the only series that does; nothing wants a subset.

The module must be built for, and run on, a kernel with netmem TX
(`netdev->netmem_tx`, `netmem_dma_unmap_addr_set()` and
`netmem_dma_unmap_page_attrs()` in `<net/netmem.h>`). v7.2.6 and v7.2.15
have them.

| File | Touches | Upstream? |
| --- | --- | --- |
| `0001-ionic-use-netif_txq_maybe_stop-in-ionic_tx.patch` | `ionic_txrx.c` | **Yes** — `c91b4d6e5cc3`, in v7.2.15 as `e247d033dd32` |
| `0002-ionic-dont-leave-a-failed-xdp-frame-in-the-tx-descriptor-info.patch` | `ionic_txrx.c` | No — RFC patch 1/6 |
| `0003-ionic-give-tx-buffers-their-own-bookkeeping-struct.patch` | `ionic_dev.h`, `ionic_txrx.c` | No — RFC patch 2/6 |
| `0004-ionic-unmap-tx-frags-with-the-netmem-helpers.patch` | `ionic_dev.h`, `ionic_txrx.c` | No — RFC patch 3/6 |
| `0005-ionic-count-the-unreadable-skbs-dropped-on-tx.patch` | `ionic_lif.h`, `ionic_stats.c`, `ionic_txrx.c` | No — RFC patch 4/6 |
| `0006-ionic-dont-send-an-unreadable-skb-that-has-no-frags.patch` | `ionic_txrx.c` | No — RFC patch 5/6 |
| `0007-ionic-add-netmem-tx-support.patch` | `ionic_lif.c` | No — RFC patch 6/6 |
| `0008-ionic-send-an-unreadable-skb-with-too-many-frags-as-a-tso.patch` | `ionic_lif.h`, `ionic_stats.c`, `ionic_txrx.c` | No — a seventh RFC patch, not in the posted-form series' six |

The RFC is "ionic: support netmem TX", six patches based on v7.4-rc3.
Each patch's message ends with a `[ rocm-ernic: ... ]` note saying which
RFC patch it is and how the backport differs.

`0008` is newer than the rest, and its mainline form is
[`../devmem-tx-mainline/0007`](../devmem-tx-mainline). It is last because
it is the last thing written, and because it needs `0005`'s counter and
`0007`'s netmem Tx beneath it; nothing below it depends on it.

## 0008: an over-limit unreadable skb goes out as a TSO chain

A Tx descriptor reaches 16 buffers — the one it points to and 15
scatter-gather elements. A packet in more pieces is linearized, which
cannot be done for an unreadable (net_iov) skb, so `0005`'s counter
`tx_unreadable_drop` counted it and the driver dropped it. For a devmem
TCP sender that is not a lost packet but the end of the connection: TCP
retransmits the same skb, so every retransmission has the same frags and
is dropped too, until the socket gives up with `ETIMEDOUT`. A sender
reaches it with iovec pieces below about `MSS/15`, or with unpaced
`TCP_NODELAY` sends that `tcp_sendmsg_locked()` coalesces onto the skb
at the tail of the write queue — 15 pieces of 64 bytes become an skb of
17 frags.

`0008` posts such an skb with the TSO opcode and an mss the driver
picks: the smallest sum of 15 consecutive frags, capped by the skb's
`gso_size` or its payload, so that every descriptor fits. To the device
it is an ordinary TSO; to the peer it is the same bytes in shorter
segments. The skb is not modified — `skb_shinfo` is taken as
`const`, `gso_size` and `gso_segs` are left alone and no payload byte is
read — and a new counter, `tx_unreadable_reseg` (per queue and summed),
counts the skbs sent this way.

It is done only for plain TCP over IPv4 or IPv6 with `CHECKSUM_PARTIAL`,
headers and nothing else in the linear area, no tunnel, no IPv6
extension headers, no SYN/RST/URG, IPv4 with DF set, and TSO (and
`TSO_ECN` where CWR is set) still enabled in `netdev->features`. What
does not qualify, and a layout needing more than half the ring in
descriptors, is dropped and counted in `tx_unreadable_drop` as before.

**Whether real firmware accepts an mss as small as 15, or the runt final
segment of such a chain, is unknown**: only this emulator has run it.

## The devmem Rx series goes on top of all eight

The Rx series adds its own counters to the three places `0008` touches,
directly after `0008`'s. It was regenerated on top of this series as it
stands, so a Tx+Rx guest applies this whole directory and then
[`../devmem-rx-7.2.6`](../devmem-rx-7.2.6), with no file left out and no
second form of anything.

It was not always so: `0008` was once the last patch of a Tx+Rx tree,
carried twice under a `devmem-tx/on-devmem-rx/` that nothing could
apply, because `fetch-ionic-sources.sh` applies every `*.patch` in a
directory it is given and the Rx series would not go above `0008`.
Keep it this way: if `0008` has to move, move it inside this series and
regenerate the Rx series on the result rather than making a second copy
of it.

## How the backport differs from the RFC

Hardly at all, because of 0001. v7.2.6's `ionic_txrx.c` differs from
v7.4-rc3's by one commit, which v7.2.15 has and v7.2.6 does not: the fix
to the Tx queue stop in `ionic_tx()`. It is carried as 0001, unchanged.
With it `ionic_txrx.c` is the file the RFC was written against, and:

- 0002, 0003, 0004 and 0006 are the RFC's diffs unchanged;
- 0005 leaves out the RFC's change to
  `Documentation/networking/device_drivers/ethernet/pensando/ionic.rst`,
  which the sparse checkout doesn't have;
- 0007 differs only in its line numbers.

0001 is a fix in its own right (a queue stopped without the barriers
`netif_txq_maybe_stop()` has), and it changes the Tx path of every guest
built with this series whether or not it uses devmem.

RFC patch 5/6 (0006) is marked not for net-next, where the core no
longer makes the skbs it drops. v7.2.15 does make them, so it stays.

## What the guest needs besides this

`netmem_tx` is set only when the device grants Tx scatter-gather
(`IONIC_ETH_HW_TX_SG`) and Tx checksum (`IONIC_ETH_HW_TX_CSUM`) and its
Tx queues have scatter-gather elements. A rocm-ernic server that grants
no offloads gives a guest whose bind fails with `EOPNOTSUPP`; see
"Transmit offloads" in [`docs/ionic.rst`](../../../../docs/ionic.rst).
The guest's kernel needs `CONFIG_NET_DEVMEM`, and something to make a
dma-buf from, such as `CONFIG_UDMABUF`.

## Applying it

The series is **optional**, so it is not in `patches/`, where
[`fetch-ionic-sources.sh`](../../scripts/fetch-ionic-sources.sh) picks
the base series up by default. Name every series, in order, this one
after `srq/` and `p4/` — and
[`../devmem-rx-7.2.6`](../devmem-rx-7.2.6) after it, if the guest wants
devmem receive too:

```bash
ansible/files/scripts/fetch-ionic-sources.sh \
    --source-dir /path/to/ionic-src \
    --kernel-ref v7.2.6 \
    --patches-dir ansible/files/patches/srq \
    --patches-dir ansible/files/patches/p4 \
    --patches-dir ansible/files/patches/devmem-tx
```

With `ernic_guest_setup`, which applies the base series first, list it in
`ernic_guest_ionic_extra_patches`:

```yaml
ernic_guest_ionic_extra_patches:
  - patches/devmem-tx
```

No role, package or CMake target applies it by default.

A guest that also builds `ernic_p4.ko` must build it against the same
tree: 0005 adds members to structures in `ionic_lif.h`, so add this
directory to `ernic_p4_dkms_patches_dirs` as well.

## What was tested

- The whole of `0001-0008` applies in order, with `git apply --check`
  and `git apply --whitespace=error-all`, to v7.2.6 alone, with the base
  series, with base and p4, with srq alone, and with srq and p4 — the
  five bases a caller or CI uses. `git mailinfo` parses every file.
  `git am` itself was not run; the `ionic-patches` CI job runs it, on
  top of the base series.
- `ionic.ko` and `ionic_rdma.ko` build after every patch with the Kbuild
  `setup-ionic-dkms.sh` writes, `W=1`, gcc 15.2, with no warning, against
  a v7.2.15 kernel tree configured with `NET_DEVMEM`, `UDMABUF` and
  `DMA_API_DEBUG`.
- The module was loaded in two guests of this emulator, on a v7.2.15
  kernel, and sent from a dma-buf: a 360 s run moved 64.4 GiB with every
  byte verified at the receiver and all 69,148 zerocopy completions
  returned, none reporting a copy. Unaligned offsets and lengths, up to
  17 frags per skb, MTU 1500 to 9198, TSO on and off, and teardown with
  sends in flight all passed; `tx-checksum` or `scatter-gather` turned
  off left nothing corrupt at the receiver. A probe confirmed every data
  skb was unreadable with `net_iov` frags, so none fell back to a copy.
- **Not tested:** `dkms build` itself, a build against v7.2.6 headers,
  ionic hardware, and a kernel with KASAN (the mainline form of the
  series was run under KASAN and lockdep on v7.4-rc3).

### 0008

`0008` was added later. The runs below used a tree reached by applying
an `on-devmem-rx/0009` form of it above the Rx series; that directory is
gone, and this file in this position now produces the same tree, byte
for byte, which was checked and re-run (next section). On a `v7.2.6`
kernel with the peer-to-peer dma-buf backport, in two guests of this
emulator, with the `v7.2.6` Rx series loaded as well:

- Seven layouts that ended in `ETIMEDOUT` without it — 15 and 512 iovec
  pieces of 64, 100 and 128 bytes at MTU 1500 and 9000 — all completed
  with the sha256 of every byte equal at the receiver, with
  `tx_unreadable_drop` unchanged and `tx_unreadable_reseg` rising.
  128-byte pieces at MTU 1500 never stalled and are the control.
- On the wire, captured before the receiver's GRO: 0 bad IP or TCP
  checksums, no segment above the chosen mss, PSH and FIN only on the
  last segment of a chain, contiguous sequence numbers, DF set. **IP IDs
  repeat across chains**, as the commit message says they can.
- A 300 s run moved 60.4 GiB with every byte verified and no
  retransmission; 38 rows of the devmem Tx matrix, an IPv6 pass, 3 %
  netem loss, a receiver ring of 64 under load, seven mid-stream MTU
  changes, 8 concurrent senders, and devmem Tx into devmem Rx all
  verified. The emulator refused nothing, including an mss of 15 and a
  95-descriptor chain.
- Still dropped, each ending in `ETIMEDOUT` with `tx_unreadable_reseg`
  unchanged, as designed: `tso off`, `tx-tcp6-segmentation off`, IPv4
  with DF clear, and a layout needing more descriptors than half the
  ring.
- `W=1` with no warning, `checkpatch.pl --strict` clean but for the
  missing `Signed-off-by:`, and a userspace model of the mss choice over
  5,000,000 random frag layouts.

  **Not tested for 0008:** ionic hardware, KASAN or lockdep, VLAN,
  tunnels, IPv6 extension headers, `TSO_ECN` off with CWR set, and the
  hwtstamp queue.

### After the one-order rework

This directory did not change: `0001-0008` are the same eight files,
with the same `sha256sum`, as before the Rx series was rebased onto all
eight of them. What was checked again, with the Rx series regenerated on
top:

- The tree `srq` + `p4` + this directory + `../devmem-rx-7.2.6` produces
  is **byte-identical** to the tree the modules that did the runs above
  were built from, and the three modules built from it have the same
  `srcversion` as the ones that were loaded then.
- `ionic.ko`, `ionic_rdma.ko` and `ernic_p4.ko` build after every one of
  the 28 patches of that order, `W=1` and sparse, gcc 15.2, with no
  warning, against the `7.2.6-devmemp2p` build tree. (`srq` `0001-0003`
  do not compile: `0001` uses `rcq_sign_bit`, which `0004` adds. That is
  how the SRQ series arrived and is not changed here.)
- The two frag-limit layouts that stall without `0008` — the unpaced
  `TCP_NODELAY` 15-frag sender and 512 iovec pieces of 64 bytes at MTU
  1500 — were run again in both guests, before and after. Without
  `0008`: both ended short, `tx_unreadable_drop` +7 and +8, no
  `tx_unreadable_reseg` counter at all. With it: both completed, every
  byte's sha256 equal at the receiver, `tx_unreadable_drop` +0 and
  `tx_unreadable_reseg` +56 and +1920, no emulator error counter moved.

## Retiring this directory

Delete it when the base ref moves to a kernel whose ionic sets
`netmem_tx`. Before that, drop 0001 when the base ref moves to a release
that has it, as v7.2.15 does (it then stops applying), and replace
0002-0008 when the series that lands upstream differs from the RFC.
