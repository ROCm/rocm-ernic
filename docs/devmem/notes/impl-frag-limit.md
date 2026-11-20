# ionic devmem TX: the 16-buffer frag limit (patch 7 of the TX series)

Written 2026-11-19 (local), no-stack phase: design, code, compile. **Nothing was run on a
guest**; every runtime claim below is from reading code or from a userspace model, and the
test plan in section 7 is NOT RUN. `F` = `/var/tmp/ionic-devmem/overnight2/frag`.

## 1. Result

One additional patch, "ionic: send an unreadable skb with too many frags as a TSO":
an unreadable skb that needs more than 16 buffers in one packet (non-GSO with 16+ frags, or
a GSO skb with an over-full segment) is posted with the TSO opcode and an mss the driver
picks so that every descriptor fits. The skb and its shared shinfo are not modified and no
payload is touched. What cannot be sent that way is dropped and counted as before.

| What | Path |
| --- | --- |
| Mainline form (v7.4-rc3 + TX 0001-0006), `[RFC PATCH net-next 7/7]` | `F/patches/0007-ionic-send-an-unreadable-skb-with-too-many-frags-as-a-tso.patch` |
| Same, for a tree that also has the RX series (context differs, see 6) | `F/patches/on-rx/0007-...patch` |
| v7.2.6 backport, goes after `ansible/files/patches/devmem-tx` 0001-0007 | `F/patches/v7.2.6/0008-...patch` |
| Commit message, backport note | `F/msgs/0007.txt`, `F/msgs/0007-v7.2.6.note` |
| The edit as a script (applies the change to any tree with the TX series) | `F/scripts/edit0007.py` |
| mbox builder, 7.2.x container build, userspace model, test plan | `F/scripts/{mkpatch.sh,build-726-in-container.sh,reseg_sim.c,test-frag.sh}` |
| Evidence | `F/evidence/` (`verify.txt`, `reseg-sim.txt`, `build-v7.2.6-backport.{txt,log}`, the three raw diffs), `F/build/{w1,sparse}-s7.log`, `F/build-rx/w1-sparse-s7.log` |

All three are hand-built mbox files, `From: Stephen Bates <sbates@raithlin.com>`, no
Signed-off-by. No commit was made anywhere.

Diffstat (mainline): `ionic_txrx.c` +113 -10, `ionic_stats.c` +3, `ionic_lif.h` +2,
`ionic.rst` +2. The code change is identical in the three forms (checked by diffing the
`+`/`-` lines); the backport only lacks the `ionic.rst` hunk, like devmem-tx 0005.

## 2. Designs considered

### (a) Driver: re-segment with the device's TSO. CHOSEN.

`ionic_tx_tso()` already emits one descriptor per segment, each with its own SG list, and
splits frags by address arithmetic. The patch gives it the mss as a parameter instead of
reading `gso_size`; `ionic_tx_descs_needed()` returns the mss through a pointer (the skb's
`gso_size`, 0 for a plain packet, or the chosen one) and both xmit paths do
`if (mss) ionic_tx_tso(..., mss) else ionic_tx()`.

**Uniform mss, not "fill each descriptor".** Two shapes were possible: (1) descriptors of
unequal payload, 16 buffers each (2 descriptors for any 17-frag skb), or (2) a normal TSO
chain with a smaller mss, every descriptor but the last carrying exactly mss. `ionic_if.h`
defines mss as "the TCP payload length for each segment", and the Linux driver has only ever
posted shape (2), so a device that computes sequence numbers as `index * mss` would corrupt
shape (1). The emulator would take either (it sums real lengths, `ionic_eth_emu.c:2353-2355`);
real firmware is unknown. Shape (2) is what the device already sees for any small `gso_size`.

**The mss** is the smallest sum of `max_sg_elems` (15) consecutive frags, capped by the skb's
own `gso_size` (GSO) or payload (non-GSO). Why it is safe: a descriptor needing a 17th buffer
contains 15 whole frags plus at least one byte on each side, so more than that sum; the
first descriptor (headers + 15 frag buffers) overflows only with the first 15 frags plus one
byte. O(nr_frags), no search. Zero-length frags, which would break the argument, are refused.
`F/scripts/reseg_sim.c` models the choice and the `ionic_tx_tso()` walk: 5,000,000 random
layouts (up to 45 frags, SG limits 1-15, GSO and not): 0 with more than `max_sg + 1` buffers,
0 where an over-full layout was not given a smaller mss (`F/evidence/reseg-sim.txt`).

| Layout | payload | mss chosen | descriptors |
| --- | --- | --- | --- |
| 16 x 90 B | 1440 | 1350 | 2 |
| 17 x 64 B (the coalesced 15-frag pieces, F1) | 1088 | 960 | 2 |
| 17 x 1 B | 17 | 15 | 2 |
| 15 x 96 B + 4096 B, gso 1448 | 5536 | 1440 | 4 (as before) |
| 1400 B + 16 x 1 B | 1416 | 15 | **95** |

The last row is the price of no search: 3 segments of 700 would do. Capped at half the ring.

**Answers to the questions asked of (a):**

- *shinfo untouched:* yes. `ionic_tx_unreadable_reseg()` takes `const struct
  skb_shared_info *`; `gso_size`/`gso_segs` are not written.
- *skb_cow_head and the header edit on a non-GSO skb:* allowed. It is what the TSO path does
  to every GSO clone. TCP's clone owns its header (`__skb_header_release`), so
  `skb_cow_head()` is a no-op and the retransmit queue's copy never sees the bytes (TCP
  rewrites all headers on each transmit). If a tap also holds a clone, `pskb_expand_head()`
  copies the head; it works for net_iov frags (`SKBFL_DONT_ORPHAN`, `skb_frag_ref`).
- *User turned TSO off:* then the driver told the device to disable it
  (`ionic_set_nic_features()`), so the opcode may be refused. The patch requires
  `NETIF_F_TSO` / `NETIF_F_TSO6` in `netdev->features` (and `NETIF_F_TSO_ECN` if CWR is
  set); otherwise the skb is dropped and counted in `tx_unreadable_drop` as today. **This is
  the main limitation that remains.**
- *TCP only:* `CHECKSUM_PARTIAL`, `csum_start` at the transport header, `csum_offset` of
  `tcphdr.check`, IPv4 protocol / IPv6 nexthdr TCP (no extension headers), headers exactly
  filling the linear area, no SYN/RST/URG.
- *IP IDs:* the device numbers segments from the header's ID; the stack reserved only one
  (or `gso_segs`). Harmless for atomic datagrams, so IPv4 must have DF and no fragment bits.
- *PSH/FIN/CWR:* device rule (FIN, PSH last; CWR first), documented in `ionic_if.h:875-899`
  and implemented in the emulator (`ionic_eth_tx.c:183-190`).
- *Receiver / sender TCP:* same byte stream in shorter segments. A partial ACK of a
  `pcount == 1` skb is the "middlebox resegmented" case: `tcp_clean_rtx_queue()` leaves the
  skb (`tcp_input.c:3657-3660`) and a retransmit trims at `snd_una`
  (`tcp_output.c:3632-3643`, `tcp_trim_head()`, frag arithmetic only).
- *BQL / stats:* unchanged from TSO: `netdev_tx_sent_queue(skb->len)` once at the first
  descriptor, one completion for the skb. `tx_pkts` counts segments, `tx_tso` and
  `tx_tso_bytes` count the skb, new `tx_unreadable_reseg` (per queue and total) counts skbs
  sent this way. The counter is bumped in `ionic_tx_tso()`, not in the dry run, so a
  `NETDEV_TX_BUSY` retry does not double count.
- *hwtstamp, encap:* excluded (dropped and counted as before).
- *GSO skb with an over-full segment:* yes, same function; its mss comes out below
  `gso_size` and `ndescs = DIV_ROUND_UP(payload, mss)` replaces `gso_segs`.

### (b) Core: bound the frags of a devmem skb per device. Proposal only.

Would need a per-netdev limit (say `dev->tx_max_frags`, copied to the socket in
`sk_setup_caps()` like `sk_gso_max_segs`) honoured everywhere a to-be-sent unreadable skb
gains frags (v7.4-rc3 line numbers):

| Site | What grows |
| --- | --- |
| `net/core/datagram.c:727` `zerocopy_fill_skb_from_devmem()` | new skb and appends from `tcp_sendmsg_locked()` (`tcp.c:1256`, `:1353`); bound is `MAX_SKB_FRAGS`, `net.core.max_skb_frags` is not consulted (`tcp.c:1305` is the copy path only) |
| `net/core/skbuff.c:4395` `skb_shift()` via `tcp_grow_skb()` (`tcp_output.c:2988-3000`) | merges the next unsent skb into the head; `tcp_skb_can_collapse()` allows unreadable + unreadable (`include/net/tcp.h:1264-1271`) |
| `tcp_output.c:2663`, `:2702` `tcp_mtu_probe()` / `tcp_clone_payload()` | builds a probe sharing frags, up to `MAX_SKB_FRAGS` |
| `skb_shift()` via `tcp_shifted_skb()` (`tcp_input.c:2012`, `:2084`, `:2105`) | SACKed skbs only; never resent, harmless |
| `tcp_collapse_retrans()` | already refused for unreadable skbs (`tcp_output.c:3561`) |

A per-skb cap of 15 frags is enough for GSO too (no segment can then touch 17 chunks).
Judgement: (b) is the cleaner long-term answer, since every netmem TX driver with a
per-packet buffer limit has the same hole (gve drops too), and it is worth raising in the
cover letter. But it is five sites in TCP for one driver's limit, cannot be backported to a
DKMS module, and (a) costs nothing once (b) exists. So (a) is the patch; (b) is a discussion
item.

### (c) `ndo_features_check` / `netif_set_tso_max_*`. Refuted, as before.

`features_check` can only remove features; removing GSO makes the core segment, which gives
packets with the same frags (and fails for unreadable payload when it must copy); removing
SG asks the core to linearize, which it cannot. Neither splits a non-GSO skb.
`tso_max_segs/size` bound segments and bytes per GSO skb, not buffers per segment.

## 3. What is still dropped (counted in tx_unreadable_drop, connection still dies)

1. TSO off or not granted for that IP version (or TSO_ECN off with CWR set).
2. Layouts needing more than `num_descs / 2` descriptors (ring 1024: mss below about 1/512
   of the payload; ring 64: more than 32).
3. Not plain TCP: tunnels, IPv6 extension headers, in-band VLAN header, IPv4 without DF,
   SYN/RST/URG, payload in the linear area, the hwtstamp queue. None is known to occur for
   devmem TCP.

## 4. Emulator vs hardware (emulator read at 2d3a3d02, not edited)

- The emulator carries what the patch emits: TSO chain rules (`ionic_eth_emu.c:2391-2452`)
  want SOT first, EOT last, same `hdr_len`/`mss` in every descriptor, `mss >= 1`,
  `hdr_len <= 256`, per-descriptor payload 1..mss, total payload <= 65535, <= 15 SG
  elements, unfragmented IPv4 or IPv6 TCP. An mss of 15 and a 95-descriptor chain pass.
- **Emulator more permissive than hardware may be:** it does not keep the enabled feature
  set ("Nothing is kept", `ionic_eth_emu.c:1550-1559`), so it would segment a TSO chain
  even with `tso off`. The patch does not rely on that (it checks `netdev->features`), so
  test L1 shows the driver's check, not the device's.
- **Emulator stricter than hardware:** it refuses VLAN and ENCAP flags on a TSO descriptor
  (offloads it does not grant), so a VLAN-tagged over-limit skb cannot be exercised here.
- **Unknown on hardware:** whether firmware accepts an mss as small as 15 (TCP itself never
  asks for less than `TCP_MIN_GSO_SIZE` = 8, so the value is legal for the stack), and
  runt final segments. Stated in the commit message.

## 5. Checks run

| Check | Result |
| --- | --- |
| `checkpatch.pl --strict` on the three mbox files | 0 warnings, 0 checks; only "Missing Signed-off-by" (`F/evidence/verify.txt`) |
| W=1, v7.4-rc3 + 6 + 0007, own build dir `F/build` seeded from `build-74/.config` | rc 0, 13 CC, 0 warnings (`F/build/w1-s7.log`) |
| sparse (`C=2`, `tools/sparse-src/sparse`), same tree | 14 CHECK, 0 warnings (`F/build/sparse-s7.log`) |
| W=1 + sparse on the RX tree state + 0007 (`F/linux-rxchk`, `F/build-rx` from `build-74-rx/.config`) | rc 0, 13 CC, 0 warnings |
| v7.2.6 + srq + p4 + devmem-tx + 0008, W=1, `devmem-kbuild:1` (gcc 15.2), against the built 7.2.15-devmem kernel | 24 CC, 4 LD, 0 warnings, `ionic.ko` vermagic 7.2.15-devmem (`F/evidence/build-v7.2.6-backport.*`) |
| Apply checks | mainline form: applies to `linux` (check only) and reverses on my worktree; on-rx form: applies to `linux-rx` (check only); v7.2.6 form: applies to v7.2.6+srq+p4+devmem-tx and to v7.2.6+devmem-tx alone |
| Userspace model | section 2 |

NOT RUN: any guest, any traffic, a build against the guest's 7.2.6 headers (the test plan's
`stage` step does it), `git am`, KASAN, IPv6, real hardware.

## 6. Conflict with the RX series

`ionic_txrx.c` applies to the RX tree with offsets only (+181 lines). **Three context
conflicts**, all trivial: the RX series adds `rx_hds_split`, `rx_hds_hbo`,
`rx_unreadable_drop` directly after `tx_unreadable_drop` in `struct ionic_lif_sw_stats`
(`ionic_lif.h`), `ionic_lif_stats_desc[]` (`ionic_stats.c`) and the `ionic.rst` example,
exactly where this patch adds `tx_unreadable_reseg`. Either apply `patches/on-rx/0007` on
top of the RX series, or put 0007 into the TX series and refresh the context of RX patches
that add those lines. A v7.2.6 RX backport does not exist yet
(`ansible/files/patches/devmem-rx` is v7.4-rc3); when it does, expect the same three
context conflicts and nothing else. `edit0007.py` regenerates the change on any such tree.

## 7. Test plan for the stack engineer (NOT RUN)

Script: `F/scripts/test-frag.sh {stage|control|pos|limits|restore}`; it sources the
verifier's `v.sh` (vtx random payload + expected file, `vrx.py` sha256, `vprobe.bt`).
Stack 3 must be up with the devmem ionic loaded, as the verification note left it.

1. `test-frag.sh control` on the current module (`ionic-mod.sh load devmem`): reproduces F1.
   Expect sender `ETIMEDOUT`, receiver 0 bytes, `tx_unreadable_drop` > 0.
   `sudo vtx -e /tmp/vtx.exp -S 4m -R 300 -N -t 5 -C 2 rocm-ernic0 10.102.100.12 6300 d:9:960:64:4032:4096`
2. `test-frag.sh stage`: `ionic-mod.sh stage frag srq p4 devmem-tx F/patches/v7.2.6`,
   `build frag`, `load frag`. Assert `ethtool -S` lists `tx_unreadable_reseg` and
   `tx_N_unreadable_reseg`.
3. `test-frag.sh pos`, nine cases (P1 = the control's send, P2 16 frags, P3 17 x 16 B,
   P4 back-to-back 1-byte sends, P5 80-byte pieces, P6 1400 B + 16 x 1 B (95 descriptors),
   P7 GSO with an over-full segment, P8 60 s sustained, P9 large-send control). Assert:
   - **receiver:** `VERDICT ... DATA-OK COMPLETE` (sha256 of all bytes), `zero_runs=0`;
   - **sender counters:** `tx_unreadable_drop` +0, `tx_unreadable_reseg` > 0 in P1-P8 and
     +0 in P9, `tx_tso` up by at least the reseg delta, `tx_linearize` +0, `netdev.tx_dropped` +0;
   - **vprobe:** `@unreadable_frags` has entries at 16 and 17 (the skbs really were over
     the limit; P1 without them proves nothing) and `@unreadable_dropped` is empty;
   - **emulator:** `emu.tx_desc_data_error`, `tx_desc_fetch_error`, `tx_dma_error` +0;
   - **TCP:** `TcpRetransSegs`, `TCPTimeouts` about 0, receiver `TcpInCsumErrors` +0;
   - **wire** (re-run P1 and P6 with the devmem-tx harness's capture, `CAP=1`, or
     `tcpdump -i rocm-ernic0 -vv tcp port 6310` on r102-2 with `ethtool -K rocm-ernic0 gro off`
     there, since GRO merges the segments before the tap): for a 17 x 64 B skb, two
     segments of 960 and 128 bytes, consecutive sequence numbers, PSH only on the second,
     IP IDs n and n+1, DF set, `cksum ... (correct)` on both; no segment larger than the mss;
   - no splat in either guest's dmesg.
4. `test-frag.sh limits`: L1 `ethtool -K rocm-ernic0 tso off` then 16-frag sends: expect the
   old stall with `tx_unreadable_drop` > 0 and `tx_unreadable_reseg` +0. L2 `ethtool -G
   rocm-ernic0 tx 64` then P6's layout: same. L3 positive run after restoring both.
5. Regression: `devmem-tx/m6.sh pos`, `m6.sh pos2`, `run-matrix.sh` (the negative rows M6n*
   and the verifier's `F-NEG-*` now complete instead of failing: update their expectations),
   `verify/c-offloads-off.sh` with `NOFRAG=1` (the offload-off trap must still drop),
   `qs-test.sh` base, SRQ and P4 once each.
6. `test-frag.sh restore` (back to `devmem`), or leave `frag` loaded if the plan says so.

Rates: none claimed. P6 is expected to be slow (95 packets per 1416 bytes).

## 8. State left

- Worktrees added (detached at v7.4-rc3, uncommitted changes only): `F/linux` (six TX
  patches + 0007; the six are staged in its index with `git apply --cached` so that
  `git diff` is 0007) and `F/linux-rxchk` (the `linux-rx` working state + 0007, same
  arrangement). `linux`, `linux-rx`, `linux-7.2.6`, `build-74*`, `patches/`, `patches-rx/`
  and rocm-ernic were only read.
- `F/k726/{a,b,c,dkms,repo}`: plain directories (v7.2.6 ionic from `git archive`, the repo's
  patches from `git archive HEAD`).
- Stack 3 not touched.
