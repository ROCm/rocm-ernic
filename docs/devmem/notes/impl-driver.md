# ionic devmem TX: driver series for mainline v7.4-rc3

Written 2026-11-18. Tree `/var/tmp/ionic-devmem/linux`, branch `devmem-tx`, HEAD
`aebb72744764` (v7.4-rc3). Nothing is committed: the full series is in the working tree as
uncommitted changes to six files, and as mbox files.

This run resumed an interrupted one. It kept that run's patches 1-4 after re-reading them
against the source, rewrote the runtime part of patch 5, wrote all commit messages, the cover
letter and the scripts below, and re-ran every build and check listed here.

## Where things are

| What | Path |
| --- | --- |
| Patches (git-am mbox, no Signed-off-by) | `/var/tmp/ionic-devmem/patches/mainline/000[0-5]-*.patch` |
| Commit message sources | `/var/tmp/ionic-devmem/patches/msgs/000[0-5].txt` |
| File state after each patch (`s0` = pristine) | `/var/tmp/ionic-devmem/patches/stages/s[0-5]/` |
| Patch generator | `/var/tmp/ionic-devmem/patches/scripts/mkpatches.sh` |
| Apply / mailinfo / checkpatch check | `/var/tmp/ionic-devmem/patches/scripts/verify.sh`, output `patches/verify.log` |
| Per-stage build | `/var/tmp/ionic-devmem/patches/scripts/build-series.sh`, output `patches/build.log` |
| Build logs | `/var/tmp/ionic-devmem/build-74/series-{w1,sparse}-s[0-5].log`, `/var/tmp/ionic-devmem/build-74-nomapstate/series-w1-s[0-5].log` |

To regenerate after editing a stage or a message: `bash patches/scripts/mkpatches.sh`, then
`verify.sh`. `build-stages.sh` and `edit.py` in `patches/scripts/` are leftovers of the first
run and are not used.

## Series

```
[PATCH net-next 0/5] ionic: support netmem TX
[PATCH net-next 1/5] ionic: give Tx buffers their own bookkeeping struct
[PATCH net-next 2/5] ionic: unmap Tx frags with the netmem helpers
[PATCH net-next 3/5] ionic: don't try to linearize an unreadable skb
[PATCH net-next 4/5] ionic: count unreadable skbs sent and dropped
[PATCH net-next 5/5] ionic: add netmem TX support

 Documentation/.../pensando/ionic.rst               |  4 +
 drivers/net/ethernet/pensando/ionic/ionic_dev.h    | 13 ++++
 drivers/net/ethernet/pensando/ionic/ionic_lif.c    | 52 ++++++++++++
 drivers/net/ethernet/pensando/ionic/ionic_lif.h    |  4 +
 drivers/net/ethernet/pensando/ionic/ionic_stats.c  |  6 ++
 drivers/net/ethernet/pensando/ionic/ionic_txrx.c   | 62 +++++++++-----
 6 files changed, 119 insertions(+), 22 deletions(-)
```

| # | Files | What it does |
| --- | --- | --- |
| 1 | `ionic_dev.h`, `ionic_txrx.c` | New `struct ionic_tx_buf_info { dma_addr; len; }` for `ionic_tx_desc_info.bufs[]`. Drops the XDP path's writes of `page`/`page_offset`, which nothing on Tx reads. No functional change. Rx keeps `struct ionic_buf_info`. |
| 2 | `ionic_dev.h`, `ionic_txrx.c` | Adds `DEFINE_DMA_UNMAP_ADDR(unmap_addr)`. `ionic_tx_map_skb()` sets it with `netmem_dma_unmap_addr_set()`; `ionic_xdp_post_frame()` sets it to the address (XDP_REDIRECT frag) or 0 (XDP_TX frag, page pool mapping). Both frag unmap sites (`ionic_tx_desc_unmap_bufs()`, the `dma_fail:` unwind) use `netmem_dma_unmap_page_attrs()`. Head keeps `dma_unmap_single()`. |
| 3 | `ionic_txrx.c` | `ionic_tx_descs_needed()` returns `-EOPNOTSUPP` for an unreadable skb instead of calling `skb_linearize()`. |
| 4 | `ionic_lif.h`, `ionic_stats.c`, `ionic_txrx.c`, `ionic.rst` | `tx_unreadable` / `tx_unreadable_drop` (lif) and `tx_N_unreadable` / `tx_N_unreadable_drop` (queue) in `ethtool -S`. New `ionic_tx_drop()` shared by the two xmit functions. |
| 5 | `ionic_lif.c` | `ionic_lif_set_netmem_tx()`: `NETMEM_TX_DMA` only while the device granted `IONIC_ETH_HW_TX_SG` and `IONIC_ETH_HW_TX_CSUM`, the netdev has `NETIF_F_SG` and `NETIF_F_HW_CSUM`, and `max_sg_elems` is not 0. Called at probe, around the device command in `ionic_set_features()`, and from `NETDEV_FEAT_CHANGE`. |

Patches 1-4 don't touch `ionic_tx()`, the one function that differs between v7.2.6 and
v7.4-rc3, so the backport should need line offsets only (not tried in this run).

## Design rationale

**Unmap state (patches 1, 2).** The audit's design A (mark with `buf_info->page`) was dropped
for design B, the documented helpers. Reasons, checked in source:

- `dma_addr` can't hold the marker: `ionic_tx_skb_frags()`, `ionic_tx_calc_*()` and
  `ionic_tx_tso()` read it back to build descriptors.
- XDP shares `tx_info[]` slots with skbs and nothing clears `bufs[]` on completion
  (`ionic_tx_desc_unmap_bufs()` only zeroes `nbufs`). So every writer of a frag slot writes the
  field: `ionic_tx_map_skb()` and both branches of `ionic_xdp_post_frame()`. Those are the only
  writers of `bufs[1..]` in the driver.
- A separate Tx struct means Rx buffers don't grow, and Tx doesn't either: 24 bytes before
  (page, dma_addr, page_offset, len), 16 after patch 1, 24 after patch 2.
- `bufs[0]` (head, or XDP data) has no `unmap_addr`: it is never a frag, and it is unmapped
  with `dma_unmap_single()` on `dma_addr` exactly as before. XDP_TX descriptors never reach the
  unmap function at all (`ionic_xdp_tx_desc_clean()` checks `act == XDP_REDIRECT`); the 0 is
  written anyway so the rule "every frag slot is always valid" holds.
- Without `CONFIG_NEED_DMA_MAP_STATE` the field is gone, `dma_unmap_addr()` is 0, and no frag
  unmap is called. That is the kernel-wide contract for these macros (unmap is a no-op there).
  On x86_64 the option is always selected, and ionic depends on 64BIT, so this was only
  compile-checked with a hand-edited `autoconf.h` (below).
- Same caveat as bnxt/gve/mlx5: a page really mapped at DMA address 0 would not be unmapped.

**Frag overflow (patch 3).** The driver can only drop. Evaluated and rejected:
`ndo_features_check()` (can't split a non-GSO skb; a software GSO segment of an unreadable skb
references the same frags, so it still has 16+; clearing SG moves the same failed linearize into
the core), `netif_set_tso_max_segs/size()` (bound segments and bytes, not buffers per segment),
and a core cap (none exists: `zerocopy_fill_skb_from_devmem()` only knows `MAX_SKB_FRAGS`).
Residual limitation, stated in the commit message: senders using pieces under about MSS/16
(90 bytes at MTU 1500) get those packets dropped and the connection stalls.

**Counters (patch 4).** `unreadable` is bumped in `ionic_tx_map_skb()` after all frags mapped,
when `nr_frags && !skb_frags_readable(skb)`; once per skb, so once per TSO skb. Both callers
(`ionic_tx()`, `ionic_tx_tso()`) always post after a successful map. `unreadable_drop` is
bumped in `ionic_tx_drop()` for any unreadable skb the driver frees: too many buffers, DMA map
failure, `skb_cow_head()` failure in the TSO header edit. Drops by the core (netmem_tx off,
wrong device) happen in `validate_xmit_skb()` and show up in the netdev's `tx_dropped`, not here.

**Feature gating (patch 5).** The corruption case is: `NETIF_F_SG` and `NETIF_F_HW_CSUM` both
off in `netdev->features`, GSO unreadable skb, `skb_segment()` copy path,
`skb_copy_and_csum_bits()` zero-fills, `tcp_gso_segment()` writes a valid checksum. Policy:

- Probe: set `NETMEM_TX_DMA` only if both device features were granted (gve precedent for a
  conditional set) and `max_sg_elems != 0` (it is 0 if TXQ Q_IDENTIFY failed, and then every
  skb with a frag would be dropped).
- Runtime: netmem_tx follows the features. With `NETMEM_TX_NONE`,
  `validate_xmit_unreadable_skb()` frees the skb before `netif_skb_features()` is consulted,
  and `BIND_TX` returns `-EOPNOTSUPP`.
- Ordering, which the first run's version got wrong in both directions (it changed netmem_tx
  inside `ndo_set_features()` only, after the device command): `netdev->features` is assigned by
  `__netdev_update_features()` after `ndo_set_features()` returns. Turning off: clear netmem_tx
  in `ionic_set_features()` before the device command and `synchronize_net()`;
  `validate_xmit_skb()` runs inside `rcu_read_lock_bh()` or with BH off in all three callers
  (`__dev_queue_xmit()`, `__dev_direct_xmit()`, `sch_direct_xmit()`). Turning on: raise it from
  `NETDEV_FEAT_CHANGE`, which `netdev_update_features()`, the ethtool netlink path
  (`net/ethtool/features.c:288`) and the ethtool ioctl path (`net/ethtool/ioctl.c:3587`) send
  under RTNL after the assignment. A failed device command restores from the unchanged
  `netdev->features`.
- If `register_netdevice_notifier()` failed at probe (ionic carries on without it), netmem_tx
  can be lowered but is never raised again. Fails closed.
- Not done: the firmware-reset path (`ionic_restart_lif()` -> `ionic_lif_init()` ->
  `ionic_set_nic_features()`). It runs from the deferred work or PCI reset handler without RTNL
  and under `lif->queue_lock`; netmem_tx is a bitfield sharing a word with `priv_flags`, so
  writing it there needs RTNL, which would invert the RTNL -> queue_lock order of the ndo
  paths. It is also not the corruption case: `netdev->features` is not recomputed after a reset
  either, so the stack never takes the software path. netmem_tx is re-evaluated against the new
  grant at the next feature change.
- Not done: a driver backstop that drops "unreadable but no frags" skbs. The zero-filled
  segments do carry `skb->unreadable` (copied by `__copy_skb_header()`), but relying on that
  side effect is fragile, and with the ordering above no such skb can be produced.
- The write of netmem_tx is a plain bitfield store read locklessly by the xmit path. Writers
  are serialized by RTNL; a reader sees the old or the new word.

## The four things to establish (ionic-tx-audit.md sections 1-4), after coding

1. **Checksum and SG offload.** Audit confirmed. Verification-pass V1 (valid checksum over
   zeroes, i.e. corruption, not a stall) drove patch 5. What coding added: gating at probe is
   not enough and "set it in `ndo_set_features()`" is racy, because `netdev->features` changes
   after the ndo returns; hence the lower-before / raise-after split. V5 (firmware reset) is
   left alone for the locking reason above. The core problem remains for every other netmem_tx
   driver; none of them follows the features.
2. **Frag limit vs TSO.** Audit confirmed; nothing to add to its arithmetic. The code change is
   the explicit `-EOPNOTSUPP` plus a counter. The pre-existing `max_sg_elems = 15` on a v0
   (8-element) SG descriptor is not fixed here: it is not netmem-specific, and for rocm-ernic
   it means the emulator must identify TXQ v1 with 256-byte SG descriptors or the driver will
   write up to 15 elements into an 8-element slot.
3. **Other payload touches.** Re-checked while editing; no code change needed beyond the unmap:
   no padding, copybreak or software checksum on Tx; `ionic_start_hwstamp_xmit()` uses the same
   descs-needed / map / post / clean functions (and now the same drop helper); TSO edits only
   the linear header after `skb_cow_head()`; XDP frames are page-backed and now write their
   unmap state. One correction to the audit: its table says the XDP path needs a change "only
   with design B". With any design the marker has to be written by the XDP path too, or a slot
   last used by a devmem skb and then by XDP_REDIRECT would skip an unmap.
4. **Firmware / device assumptions.** Nothing learned from coding; no code change. The silent
   52-bit address mask (a TSO segment starting inside a frag puts a frag address in `cmd`) is
   listed in the cover letter as left alone. Real firmware behaviour is still unverified.

## Is this driver-only?

For the feature, yes: no core change, no new exported symbol. The cover letter says so, and
lists three core issues: (1) software GSO of an unreadable skb sends zeroes when SG and
checksum are off (patch 5 works around it; a check in `skb_segment()` would let ionic drop the
`ndo_set_features()`/notifier part); (2) nothing bounds devmem frags by the device's SG limit
(patches 3 and 4 make it a counted drop); (3) netdev unregistration does not look at Tx
bindings (from the core notes; not reproduced, not addressed).

## What was run, and results

Host: x86_64, gcc 13.3.0 (Ubuntu 24.04), make with bison from `/var/tmp/ionic-devmem/tools/bison`.
Build dirs were configured by the first run (defconfig plus `IONIC=m`, `NET_DEVMEM=y`,
`PAGE_POOL=y`, `DMA_SHARED_BUFFER=y`, `IOMMU_DMA=y`, `SWIOTLB=y`; `build-74` also has
`DMA_API_DEBUG=y`); I checked their `.config` but did not redo the configuration.

**Apply, from pristine files** (`verify.sh`): the six files extracted with
`git show aebb72744764:<file>` equal stage `s0`; each of the five patches passes
`git apply --check` and `git apply --whitespace=error-all` in order (rc 0), and after each one
the files are byte-identical to stage `sN`; after the fifth they are byte-identical to the
linux working tree. `git am` itself was not run (it commits).

**git mailinfo**: rc 0 on all six files; Author `Stephen Bates`, Email `sbates@raithlin.com`,
subjects as listed above.

**checkpatch.pl --strict** (also with `--codespell`): cover letter clean; each patch
`total: 1 errors, 0 warnings, 0 checks`, the one error being `Missing Signed-off-by: line(s)`,
which is expected because Stephen adds his own.

**Build at every step, W=1** (`build-series.sh`; each stage's files copied into the tree, then
`make O=<dir> -j64 W=1 drivers/net/ethernet/pensando/ionic/`):

```
### W=1, build-74 (NEED_DMA_MAP_STATE=y, DMA_API_DEBUG=y)
w1 s0: rc=0 CC=13 warnings=0 errors=0 ionic.o=399472
w1 s1: rc=0 CC=13 warnings=0 errors=0 ionic.o=399160
w1 s2: rc=0 CC=13 warnings=0 errors=0 ionic.o=399224
w1 s3: rc=0 CC=13 warnings=0 errors=0 ionic.o=399256
w1 s4: rc=0 CC=13 warnings=0 errors=0 ionic.o=399456
w1 s5: rc=0 CC=13 warnings=0 errors=0 ionic.o=400480
### W=1, build-74-nomapstate (autoconf.h without CONFIG_NEED_DMA_MAP_STATE)
w1 s0: rc=0 CC=13 warnings=0 errors=0 ionic.o=399480
w1 s1: rc=0 CC=13 warnings=0 errors=0 ionic.o=399168
w1 s2: rc=0 CC=13 warnings=0 errors=0 ionic.o=398344
w1 s3: rc=0 CC=13 warnings=0 errors=0 ionic.o=398376
w1 s4: rc=0 CC=13 warnings=0 errors=0 ionic.o=398616
w1 s5: rc=0 CC=13 warnings=0 errors=0 ionic.o=399632
```

All 13 ionic objects were recompiled at every stage; no warning at the baseline or at any
patch. Only the ionic directory was built (objects and `ionic.o`); no modpost/`ionic.ko`, no
vmlinux, no full-kernel build.

**NEED_DMA_MAP_STATE off.** x86_64 selects it unconditionally (`arch/x86/Kconfig:33`) and
`CONFIG_IONIC` depends on `64BIT`, so no x86 config turns it off. `build-74-nomapstate` is the
same config with the `#define CONFIG_NEED_DMA_MAP_STATE 1` line removed by hand from
`include/generated/autoconf.h` (`.config` still says `=y`). That is a compile check of the
driver with the macros compiled away, not a consistent kernel configuration. Confirmed by
preprocessing `ionic_txrx.i` in both dirs: `build-74` has `dma_addr_t unmap_addr;` in
`struct ionic_tx_buf_info` and 6 uses; `build-74-nomapstate` has an empty member and 0 uses.

**sparse**, `C=2 W=1` in `build-74`: Ubuntu's sparse 0.6.4 is rejected by the v7.4 Makefile
(`scripts/checker-valid.sh` needs `__typeof_unqual__`), so the first run built sparse from
git master (`84ec065`) in `/var/tmp/ionic-devmem/tools/sparse-src`; I used that binary.
13 files checked at each of s0..s5, 0 warnings at every stage.

**Final state**: after the last build the tree was re-compared with the result of applying the
five patches to pristine files: identical. `git status --short` shows the six files as ` M`,
HEAD is still `aebb72744764` on `devmem-tx`.

## Not run

- No runtime test of any kind: nothing was loaded, no devmem bind or send, no `ethtool -S`
  read, no `ethtool -K` toggle, no firmware reset. The zero-fill case, the synchronize/notifier
  ordering and the counters are derived from source only.
- No `devmem.py` selftest; the cover letter has a placeholder saying results must be added
  before posting.
- No KCSAN, lockdep or DMA-API-debug run (the option is on in `build-74` but nothing was booted).
- No clang build, no other architecture, no `make htmldocs` for the `ionic.rst` change.
- No backport to v7.2.6 / build against v7.2.15.
- `git am` and `git format-patch` were not used; the mbox files are assembled by
  `mkpatches.sh` from `git diff --no-index` between stages (real blob ids in the `index`
  lines; the mbox `From ` line carries a zero commit id).

## Review round

2026-11-18. This section supersedes the text above where they differ: the series is now six
patches, the runtime netmem_tx gating is gone, and patch 5 is a not-for-net-next backstop.
The interrupted earlier run of this round had changed nothing on disk.

```
1/6 ionic: don't leave a failed XDP frame in the Tx descriptor info   (new, Fixes: 5377805dc1c0)
2/6 ionic: give Tx buffers their own bookkeeping struct
3/6 ionic: unmap Tx frags with the netmem helpers
4/6 ionic: count the unreadable skbs dropped on Tx                    (tx_unreadable_drop only)
5/6 ionic: don't send an unreadable skb that has no frags             (v7.4-rc3/backports only)
6/6 ionic: add netmem TX support                                      (probe-time gate, 10 lines)
6 files changed, 74 insertions(+), 24 deletions(-)
```

**New fact that drove the redesign:** net-next (d6816d9a93b6, fetched as `FETCH_HEAD` only, no
remote or branch added) already has the core fixes the reviewers asked for: 23fe6ac2e97a and
7fec569df355 make `skb_segment()` fail for an unreadable skb, and `net_devmem_dev_unregister()`
clears Tx bindings at unregister. None is in v7.4-rc3, netdev/net or Linus' tree. A core patch I
drafted and compile-tested was therefore deleted (`patches/net-core/README.txt`).

Findings, in the order given:

1. XDP_REDIRECT map failure leaves stale `xdpf`/`act`: FIXED, patch 1 sets them after all
   buffers are mapped. Not reproduced.
2. XDP_REDIRECT frag uses plain `dma_unmap_addr_set()`: FIXED (`netmem_dma_unmap_addr_set()`).
3. Counter predicates differ, hwstamp-full omitted: FIXED; one counter, one predicate, message
   lists every drop reason.
4. Runtime feature-following unlikely to be accepted: FIXED; removed. Patch 6 is the
   probe-time gate (`TX_SG`, `TX_CSUM`, `max_sg_elems`).
5. Plain runtime store to the `netmem_tx` bitfield: FIXED by 4; no runtime write remains.
6. "[PATCH net-next]" while build-only, base not net-next: FIXED in part. Now
   "[RFC PATCH net-next]", testing stated as fact, note-to-self gone. Patches 1-4 and 6 apply
   unchanged to net-next's ionic files. DEFERRED: rebasing and building on net-next (needs a
   checkout Stephen makes), devmem.py results.
7. Old patch 3 (`-EOPNOTSUPP`) has no effect: FIXED; dropped. `__pskb_pull_tail()` already
   fails first for an unreadable skb. Frag-limit analysis moved to patch 4 and the cover letter.
8. Driver-private counters: FIXED in part; `tx_unreadable` dropped. `tx_unreadable_drop` kept
   because the frag-overflow stall is otherwise visible only in debugfs; cover letter offers to
   rework or drop it.
9. Patch 4 message omits hwstamp-queue-full: FIXED.
10. Frag overflow posed as a question: FIXED; cover letter says it is gve's behaviour and
    proposes a core-side bound as follow-up.
11. Reverse xmas tree in `ionic_lif_set_netmem_tx()`: FIXED (function removed).
12. `features` reassigned in `ionic_set_features()`: FIXED (hunk removed).
13. "No functional change" hides the address-0 caveat: FIXED in the patch 3 message.
14. Hand-assembled mbox: FIXED for the diffstat layout; DEFERRED for the zero commit id, which
    needs `git format-patch` after Stephen commits.
15. Qdisc-side segmentation defeats the gating: FIXED. Verified the five qdiscs segment at
    enqueue. Patch 5 drops unreadable skbs with no frags in `ionic_tx_descs_needed()` (both
    xmit paths) and counts them; upstream the core fix makes it unnecessary.
16. Unregister with a live Tx binding: REJECTED for net-next (already handled there); real on
    v7.4-rc3, stated in the cover letter. DEFERRED: KASAN reproduction.
17. Firmware reset with a smaller grant: FIXED for the overclaiming comment (function gone) and
    the late `synchronize_net()` stall. The stale grant itself is pre-existing; listed as left alone.
18. First driver to rewrite `netmem_tx` at runtime: FIXED by 4.
19. Zero-frag unreadable skbs sent uncounted: FIXED by 15 and 3.

**Re-run, all on v7.4-rc3, x86_64, gcc 13.3:**

- `verify.sh`: s0 equals pristine blobs; 6/6 `git apply --check` and
  `git apply --whitespace=error-all` rc 0, each result identical to its stage; linux tree
  identical to the full series; HEAD still aebb72744764, six files ` M`. `git mailinfo` rc 0 x7.
- `checkpatch.pl --strict`: cover letter clean; each patch 1 error (missing Signed-off-by),
  0 warnings, 0 checks. `uvx codespell`: only the field name `xdpf`.
- `build-series.sh`, ionic directory, 13 objects recompiled per stage, s0..s6: W=1 in `build-74`
  0 warnings; W=1 in `build-74-nomapstate` 0 warnings; sparse `C=2` 0 warnings.

**Not run:** anything at runtime (no module load, devmem send, `ethtool -K`, XDP, KASAN/KCSAN);
no build on net-next, no clang, no other architecture, no full kernel or modpost, no v7.2 backport.
