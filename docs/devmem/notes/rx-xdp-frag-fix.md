# ionic devmem RX series: the XDP frag offset bug, fixed and proven

2026-11-19 (evening, host time), stack 2 only. Evidence: `/var/tmp/ionic-devmem/overnight/b/evidence/`
(`V/` below); scripts: `/var/tmp/ionic-devmem/overnight/b/scripts/`.

## 1. The bug

Patch 4 of the RX series ("ionic: track Rx buffers as netmem") converted the XDP frag loop in
`ionic_run_xdp()` to netmem and kept both halves of the upstream bug: the frag was filled at
offset `0` instead of `bi->page_offset`, and the sync used the *first* buffer's
`buf_info->page_offset`. At MTU 9000 the third buffer of a frame is a 1402-byte page fragment,
two share a page, and every second frame's XDP frag pointed at the other one's bytes.

## 2. Where it was fixed, and why there

**The series now states the `net` fix as a prerequisite and is regenerated on top of it.** It
does not carry corrected lines of its own.

- The fix (`patches/xdp-frag-offset/net/0001-ionic-use-the-rx-buffers-page-offset-for-xdp-frags.patch`,
  `Fixes: ac8813c0ab7d`) is a bug fix for `net` that predates the series. A netmem conversion
  that says "no functional change" must not also change which bytes an XDP frag describes.
- The tooling's base stage takes it cleanly: `v7.4-rc3` + the fix + the six netmem TX patches
  apply in that order with `git apply --whitespace=error-all` (the TX patches do not touch
  those lines), and that is now stage `s0`.
- Patch 4 now *removes* the fixed page-form lines and adds their netmem form, so it does not
  apply to a tree without the fix. A series that could be applied without the fix would be the
  bug again; this one cannot.
- Cost: the series cannot be applied on plain `v7.4-rc3` + TX any more. The cover letter says
  so and lists the fix's patch-id first among the prerequisites.

End state (stages `s4`..`s8`, `linux-rx`, module `rx6`):

```c
page_pool_dma_sync_netmem_for_cpu(rxq->page_pool,
				  bi->netmem,
				  bi->page_offset,
				  frag_len);
skb_frag_fill_netmem_desc(frag, bi->netmem,
			  bi->page_offset, frag_len);
```

Stages `s0`..`s3` have the page form of the same (`bi->page_offset` in both calls).

## 3. What changed, file by file

| what | change |
| --- | --- |
| `patches-rx/stages/s0..s3/.../ionic_txrx.c` | page form of the fix (s0 is byte-identical to v7.4-rc3 + net fix + TX series) |
| `patches-rx/stages/s4..s8/.../ionic_txrx.c`, `linux-rx/.../ionic_txrx.c` | netmem form of the fix |
| `patches-rx/mainline/0004-ionic-track-rx-buffers-as-netmem.patch` | the only patch whose change differs: 58 insertions (was 57), the hunk in `ionic_run_xdp()` |
| `patches-rx/mainline/0003-*`, `0005-*`, `0007-*` | `index` blob hashes and hunk line numbers only (they touch `ionic_txrx.c` after the changed lines' file) |
| `patches-rx/mainline/0001-*`, `0002-*`, `0006-*`, `0008-*` | `Date:` header only (every regeneration rewrites it) |
| `patches-rx/mainline/0000-cover-letter.patch` (`msgs/0000.txt`) | prerequisite paragraph (fix first, then TX series), a testing paragraph saying what was re-run on the regenerated series and that the rest ran before it, diffstat 807 insertions (was 806), `prerequisite-patch-id: 1016b6deea86...` first of seven |
| `patches-rx/scripts/mkpatches.sh` | emits the fix's patch-id before the TX series' |
| `patches-rx/scripts/verify.sh` | builds the base as v7.4-rc3 + fix + TX; prints, per stage, the uses of `bi->page_offset` and that no frag is filled at 0 |
| `test-rx/ionic-mod/rx6` | module staged from the fixed tree; differs from `rx5` in `eth/ionic_txrx.c` only |

No commit message other than the cover letter changed: patch 4 is still "no functional change".
Backups of the previous stages, messages, patches and scripts: `patches-rx/old/*.1795140450`.

The patch-id of the fix, `1016b6deea860ee6ef6278de079be4bf0973a892`, is also the patch-id of the
repository's `ansible/files/patches/0003-ionic-use-the-Rx-buffer-s-page-offset-for-XDP-frags.patch`
(same change, v7.2.6 context), so the cover letter's prerequisite names that file too.

## 4. Verify and build

| check | result | evidence |
| --- | --- | --- |
| base: v7.4-rc3 blobs + net fix + 6 TX patches, `git apply --whitespace=error-all`, vs `s0` | identical, 28 files | `V/verify-xdpfix.log` |
| each RX patch `git apply --check` and apply in order, result vs `sN` | 8/8 identical | same |
| `linux-rx` working tree vs the whole series | identical | same |
| every stage: `bi->page_offset` in both calls, no `buf_info->page_offset` in the frag loop, no frag filled at 0 | s0..s8 | same |
| `git mailinfo` on all 9 files | rc 0 | same |
| `checkpatch.pl --strict` | each patch: 1 error, "Missing Signed-off-by", 0 warnings, 0 checks. Cover letter: 7 errors, one per `prerequisite-patch-id` line (checkpatch wants a commit description for any 40-digit hash; was 6 before, for the same reason) | same |
| W=1, `build-74-rx`, s0..s8 | rc 0, 13 objects, 0 warnings at every stage | `V/build-xdpfix.log` |
| sparse `C=2 W=1`, s0..s8 | rc 0, 13 files checked, 0 warnings at every stage | same |
| module `rx6` built in both guests against 7.4.0-rc3 KASAN + lockdep | built | section 5 |
| W=1 with `PROVE_LOCKING`/`KASAN` in `build-74-rx-lockdep`, RDMA driver object build, `--codespell`, `git am`, full kernel build | NOT RUN | |

The stages were not touched after the builds; only `msgs/0000.txt` changed before the last
`mkpatches.sh` + `verify.sh` (`V/verify-xdpfix-1.log` is the run before that edit). Copies of
both logs are in `patches-rx/evidence/`.

## 5. Proof on stack 2

`scripts/xdp-case.sh MODULE`: loads MODULE in both guests, `tcp-data-split off`, MTU 9000,
rx-1 `combined 1` with `xdp_tx_frags.o` / `xdp_redirect_frags.o` (section `xdp.frags`,
`test/src/xdp/xdp_echo.c`, unchanged) attached in native mode; rx-2 sends 8000-byte UDP
datagrams at 1000/s for 10 s and checks every echo with `udp_echo_check2.py` (adapted from
`test/src/xdp/udp_echo_check.py`).

**Surprise in the first run, explained** (`V/01-xdp-before-rx5.txt`): with the original checker
the old module showed `bad=0` and half the echoes *missing*, not corrupted. A corrupted echo
keeps the UDP checksum of the original datagram, so rx-2's stack drops it
(`UdpInCsumErrors`) before the UDP socket sees it. The adapted checker therefore also reads the
echoes from an `AF_PACKET` socket on rx-2's netdev, before checksum verification, and classifies
each bad one. `UdpInCsumErrors` rose by exactly the number of bad echoes in every run.

| module | server mode | action | sent | intact | bad | where / what | evidence |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `rx5` (before) | header-split on, tcp-data-split off | XDP_TX | 7458 | 3730 | **3728** | all at payload offset 7574 (the last 426 bytes); 3722 are the previous datagram's bytes at the same offsets, 6 another datagram's | `V/02-xdp-before-rx5-wire.txt` |
| `rx5` (before) | same | XDP_REDIRECT | 8023 | 4012 | **4011** | offset 7574; 4009 the previous datagram's, 1 another's, 1 neither (seq 0: what the page held before) | same |
| `rx6` (after) | same | XDP_TX | 10000 | 10000 | 0 | | `V/03-xdp-after-rx6-wire.txt` |
| `rx6` (after) | same | XDP_REDIRECT | 10000 | 10000 | 0 | | same |
| `rx6` (after) | mode off | XDP_TX | 10000 | 10000 | 0 | | `V/30-mode-off-xdp-rx6.txt` |
| `rx6` (after) | mode off | XDP_REDIRECT | 10000 | 10000 | 0 | | same |

No echo was lost or duplicated in any wire-level run. The before runs sent fewer than 10000
because the checker with the packet socket fell behind its 1000/s pacing there; not looked
into further. `rx5` and `rx6` are the same sources apart from `ionic_txrx.c`
(`diff -rq test-rx/ionic-mod/rx5/mod test-rx/ionic-mod/rx6/mod`). No splat, lockdep armed, in
all of them.

### RX regression on `rx6`, header-split mode on

Run twice: on the boot the XDP tests ran in (`V/10`, `V/11`), and again on fresh boots after
the mode-off detour (`V/40`), which is the state left behind.

| case | result | evidence |
| --- | --- | --- |
| tcp-data-split on, ordinary pages: TCP 64 MiB both ways, sha256 equal; `rx_hds_split` and `rx_csum_unnecessary` rise by about 47700 per direction, `rx_N_dropped` 0 | PASS x2 | `V/10-hds-pages-tcp-rx6.txt`, `V/40-closing-run-fresh-boot-rx6.txt` |
| devmem RX first light, 64 MiB, `ncdevmem -n -l -v 7 -L` on rx-1: 67108864 bytes, 46365 / 46360 dmabuf cmsgs, 0 linear cmsgs, 0 validation failures, exit 0; data skbs unreadable, linear 52, `ip_summed` 1 | PASS x2 | `V/11-devmem-rx-final-rx6.txt`, `V/40` |
| devmem TX (rx-2) -> devmem RX (rx-1): 700000 bytes in 100000 sends, 100000 completions, 100000 dmabuf cmsgs, 0 linear, 0 failures | PASS x2 | same |
| small-write TSO sender -> devmem RX, 2100000 bytes | PASS x2 | same |
| KASAN, lockdep, "stalled pool", "hw csum failure" | 0, `debug_locks 1` on both guests, both boots; only the known DMA-API sg-segment warning | same, `V/50-final-state.txt` |

Two things in those logs that need a word:

- `V/11` ends with "dmabuf or orphan pool still present": seven pools with no ifindex, ids 71
  to 755, `inflight 0`. They are the pools leaked earlier in that 22-hour boot by the known
  `ionic_reconfigure_queues()` leak (P1); `test-rx/evidence/review/R40-final-state.txt` lists
  755, 752 and 683 on the netdev before this work. My module reloads unregistered that netdev
  and so orphaned them. None has an id above R40's newest pool (824), so none is new. On the
  fresh boot (`V/40`) there are 0 orphans.
- `V/40` case A counts one `rx_0_unreadable_drop` and bpftrace shows one readable 242-byte
  frame with `CHECKSUM_COMPLETE` during the bind. That is the documented behaviour for a
  non-TCP frame over 256 bytes reaching a bound queue (test-rx.md, "Other observations"); I
  did not capture which frame it was. The 64 MiB stream is complete and validated.

## 6. Section 5 of test-rx.md: items settled

- **"rx-2 as devmem receiver": RUN, PASS** (`V/20-rx2-as-receiver-rx6.txt`,
  `scripts/rx2-receiver.sh`). rx-2 `combined 1`, split on: 64 MiB from rx-1 (46405 dmabuf
  cmsgs, 0 linear, 0 failures), and devmem TX on rx-1 -> devmem RX on rx-2 (100000 sends, 0
  linear, 0 failures). The first try of the second case failed in the tool, not the driver:
  ncdevmem's client refuses a netdev with one queue ("number of device queues is too low") and
  rx-1 had `combined 1`; it passed with rx-1 at `combined 4`. rx-2 is back at `combined 4`,
  rx-1 at `combined 1`.
- **"XDP rounds in mode off were not repeated in the review round": RUN, PASS**
  (`V/30-mode-off-xdp-rx6.txt`). Stack 2 restarted with `server-args none`, `rx6` loaded:
  the multi-buffer echo above, and one round of `guest/opslock.sh` on rx-1, whose XDP steps
  (attach, channels/MTU/ifdown/ifup with XDP, detach) all returned 0 this time; RESULT PASS,
  lockdep armed. Then back to `--nic-profile header-split`.
- Not touched: a firmware that really stops, a device-reported link flap, live padded/HBO/
  option frames, an application holding its tokens, the stock selftest, C2 (the ifdown reset),
  P1 and P2. All still as test-rx.md says.

Seen on the way, not followed up: the DMA-API "sg segment" warning appears once per boot per
guest here, not at every bind (dma-debug prints only its first error by default), so "no other
DMA-API report" in these runs means less than it reads.

## 7. State left

Stack 2 (`ionic-devmem-rx`) running, server `rx-hds-2` with `--nic-profile header-split`,
guests freshly booted (two cold cycles of stack 2 only, through its `stack.sh`), module
**`rx6`** + its `ionic_rdma` loaded in both, tcp-data-split on, MTU 1500, rx-1 `combined 1`,
rx-2 `combined 4`, no XDP program, no ncdevmem (`V/50-final-state.txt`). Reload after a reboot:
`test-rx/scripts/ionic-mod.sh load rx6`. Stacks 1 and 3 were not touched. No repository was
modified; nothing was committed.

## 8. For the engineer folding this into the repository

Copy, replacing the files of the same name in `ansible/files/patches/devmem-rx/` (all nine: the
regeneration rewrites every `Date:` line, and four differ in more than that):

```
/var/tmp/ionic-devmem/patches-rx/mainline/0000-cover-letter.patch
/var/tmp/ionic-devmem/patches-rx/mainline/0001-ionic-run-the-netdev-ops-under-the-instance-lock.patch
/var/tmp/ionic-devmem/patches-rx/mainline/0002-ionic-add-placeholder-definitions-for-rx-header-split.patch
/var/tmp/ionic-devmem/patches-rx/mainline/0003-ionic-trust-the-devices-l4-checksum-on-header-split-queues.patch
/var/tmp/ionic-devmem/patches-rx/mainline/0004-ionic-track-rx-buffers-as-netmem.patch
/var/tmp/ionic-devmem/patches-rx/mainline/0005-ionic-put-headers-in-the-linear-area-on-header-split-queues.patch
/var/tmp/ionic-devmem/patches-rx/mainline/0006-ionic-support-the-tcp-data-split-ring-parameter.patch
/var/tmp/ionic-devmem/patches-rx/mainline/0007-ionic-keep-an-rx-queue-going-when-its-buffers-run-out.patch
/var/tmp/ionic-devmem/patches-rx/mainline/0008-ionic-add-queue-management-ops-and-allow-unreadable-rx-payload.patch
```

sha256 of each: `V/patches.sha256`. They contain no host names, addresses or home paths
(grepped). Then, by hand, in `ansible/files/patches/devmem-rx/README.md`:

- "Base": the base is `v7.4-rc3`, then the XDP frag offset fix (the mainline form of
  `../0003-ionic-use-the-Rx-buffer-s-page-offset-for-XDP-frags.patch`; same patch-id), then
  `../devmem-tx` in its mainline form.
- Replace the section "Carries a bug the base series fixes" with a short "Needs the XDP frag
  offset fix" section: `0004` does not apply without the fix, on purpose; what is still true
  from the old text is that the fix is not upstream, so a rebase onto a newer mainline has to
  bring it along.
- "What was tested": add the multi-buffer XDP echo (XDP_TX and XDP_REDIRECT, MTU 9000, 8000-byte
  datagrams, all intact; half corrupted before), and that only the cases of section 5 above
  were re-run on the regenerated patches.
- The table row for `0004` ("No functional change") stays true.

A commit message for that change is prepared in
`/var/tmp/ionic-devmem/overnight/b/commit-msg-devmem-rx-xdp-frag-fix.txt`. The repository's
`CHANGELOG.md` entry for the series (UNRELEASED, "The ionic driver series that mode was
developed against") does not mention the bug, so it stays true as written; whether to add a
line is the repository's call. At the time of writing the repository had another engineer's
untracked `ansible/files/patches/devmem-tx-mainline/`: if the mainline TX series now lives
there, the README's "Base" link should point at it.
