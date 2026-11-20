# ionic devmem RX on emulated ionic: integration run ("first light")

Run 2026-11-18 on stack 2. **First light reached**: a guest's `ncdevmem -l -v 7 -L` received
TCP payload in its dma-buf through the ionic RX series and the emulator's header-split mode,
from a plain TCP sender and from the other guest's devmem TX, with every byte validated and no
`SCM_DEVMEM_LINEAR` cmsg. Evidence is under `/var/tmp/ionic-devmem/test-rx/evidence/` (`E/`
below); `E/LOG.md` is the running log.

## 1. Stack 2 (left running)

| Item | Value |
| --- | --- |
| Directory | `/var/tmp/ionic-devmem/test-rx/` (`scripts/`, `guest/`, `stack/`, `images/`, `kbuild/`, `ionic-mod/`, `evidence/`) |
| Compose project / stack dir | `ionic-devmem-rx` / `test-rx/stack/ionic-devmem-rx` (overlay `stack/rx-fleet.xml`) |
| Guests | `rx-1`, `rx-2`; disks `images/rx-{1,2}.qcow2` on `images/rx-backing.qcow2` (a copy of stack 1's backing file) |
| SSH | `test-rx/scripts/gssh 1` / `gssh 2`, i.e. `ssh -i test-rx/images/keys/id_rsa -p 2271` (rx-1), `-p 2272` (rx-2), `ubuntu@127.0.0.1` |
| Networks | `ionic-devmem-rx_fleet` 172.31.0.0/16 (mesh port 6332), `ionic-devmem-rx_l2`; guest addresses 192.168.100.11/.12 on `rocm-ernic0` (same as stack 1, separate L2) |
| Server | `test-rx/ernic/rx-hds-2/build/rocm-ernic` (the `rocm-ernic-rx` tree after the review round; `rx-hds-1` until then) with `--nic-profile header-split` (`stack/server-binary.path`, `stack/server-args`); log shows `NIC Rx header split: on` |
| Guest kernel | `7.4.0-rc3-devmem-kasan-vlan`: stack 1's 7.4-rc3 KASAN + lockdep source and config plus `VLAN_8021Q=y`, `INFINIBAND_IONIC=m`, built in `test-rx/kbuild/7.4-rc3-vlan`; `kasan_multi_shot` on the command line |
| Driver now loaded | `~/ionic-mod/rx5/ionic.ko` + `ionic_rdma.ko` = `linux-rx` working tree (TX series + the 8-patch RX series of the review round; `rx3` until then), in both guests; rx-1 has `combined 1`, both have `tcp-data-split on` |

- Stack 1 (`ionic-devmem`) was not stopped, restarted or reconfigured. I ran two read-only
  commands in its guest 1 by mistake (`grep hostfs /etc/fstab`, `mount`).
- A guest reboot returns to the kernel's in-tree ionic. Reload with
  `test-rx/scripts/ionic-mod.sh load rx5`; in-tree `ionic_rdma` autoload is blacklisted in the
  guests (`/etc/modprobe.d/zz-rx-test-ionic-rdma.conf`) so the pair always matches.
- Mode off and back: `scripts/stack.sh server-args none` / `server-args --nic-profile
  header-split` (each restarts stack 2 only, about 50 s).
- First light by hand: `scripts/td-setup.sh`, then `scripts/final.sh`.
- Left behind: exited container `devmem-rx-kbuild`; the 12 GB kernel build tree (`test-rx/kbuild`).

## 2. What works

True at the end of the review round (driver `rx5`, server `rx-hds-2`); evidence `E/` for the
first run, `E/review/` (`R..`) for the review round.

- Ops locking (patch 1) with `ionic_rdma` loaded, under lockdep, idle and under bidirectional
  iperf3, with shared and with split interrupts: no report, no hang, lockdep still armed.
  Also under lockdep, through test-only knobs: the driver's FW down/up handlers, the Tx timeout
  work and the link-down branch of the link check (the emulated firmware stayed alive).
- Header split with ordinary pages: headers linear, payload in frags, `CHECKSUM_UNNECESSARY`,
  GRO to 64K, for IPv4, IPv6 and VLAN; everything else arrives unsplit and correct.
- Devmem RX: 1 MiB to 1.5 GB streams validated, over IPv4, IPv6 and a VLAN, at MTU 1500 and
  9000; devmem TX -> devmem RX; two queues with one bound; split interrupts; the binding
  survives ifdown/ifup, ring resize, MTU change, a PCI function reset and a (simulated) FW
  down/up in mid-stream.
- A dma-buf smaller than the ring (ring 16384, or ring 8192 at MTU 9000, on ncdevmem's 16000
  pages): the queue receives with what it has and `rx_N_alloc_err` counts. A ring that got no
  buffers at all is filled within 10 ms of buffers appearing, without any Tx.
- Queue-op failures (injected): a failed start at bind leaves the old queue running; a double
  failure leaves the netdev present and `ifdown/ifup` recovers it; a FW reset that begins
  inside a bind or unbind ends with the queue on the right provider.
- 90 s of bind/unbind against ring, channel, MTU changes, ifdown/ifup, PCI resets and
  netlink dumps in parallel: no splat, lockdep armed, netdev and devmem RX work afterwards.
- Teardown: kill, unbind, ifdown, module unload with a live binding leave no page pool, no
  "stalled pool shutdown", no KASAN or lockdep report.
- Mode off, or tcp-data-split off, behaves as before the series (`tcp-data-split off` on a
  device without the feature is refused again, as before the series).

## 3. Cases

Guests on 7.4.0-rc3 KASAN + lockdep throughout. "splat" = KASAN, lockdep, WARN, hung task,
"hw csum failure", page-pool leak messages.

### Ops locking (step 2)

| Case | Result | Evidence |
| --- | --- | --- |
| Stock v7.4-rc3 ionic on the mode-on server: TCP 64 MiB both ways, `rx_csum_complete` rises, no "hw csum failure" (K2 byte order) | PASS | `E/01-stock-driver-*.txt` |
| Series module loads | FAIL then PASS after fix (bug 1) | `E/02-load-rx1.txt` |
| `guest/opslock.sh`, 2 rounds on each guest: ifdown/ifup x6, ring 512/4096, channels 1/2/4, MTU 9000/1500, XDP attach, channels and ifdown/ifup with XDP, detach, `ethtool` get_link/-c/-C/-a/-A/-x/-X/-s/--show-fec/--set-fec/-r/-m/-S/-d/-k/-K, promisc, allmulti, multicast and MAC address changes, `rdma link`, PCI function reset up and down | PASS, no splat, `debug_locks 1` | `E/10-opslock-rx2-guest{1,2}.txt` |
| Same, one round under bidirectional iperf3 | PASS | `E/11-*.txt` |
| Same, mode off (one round) and with tcp-data-split on (one round) | PASS | `E/51-*.txt`, `E/52-*.txt` |
| rmmod `ionic_rdma` + `ionic` and reload x3 under incoming traffic | PASS for hangs and lockdep; one DMA-API warning, pre-existing (bug P1) | `E/12-*.txt` |
| hwtstamp config | RUN: `-EOPNOTSUPP` from tsconfig netlink and `SIOCSHWTSTAMP` (emulator offers no timestamps); the ionic hwstamp queue code did not run | `E/10-*.txt` |
| Link flap | NOT RUN: the emulator has no way to drop link | |
| FW reset through the heartbeat (`ionic_lif_handle_fw_down`) | NOT RUN: the emulator cannot stop the heartbeat. The PCI function reset runs `reset_prepare`/`reset_done` (`ionic_lif_deinit_locked`, `ionic_restart_lif`) instead | |

### Header split with ordinary pages (step 3)

| Case | Result | Evidence |
| --- | --- | --- |
| `ethtool -G tcp-data-split on`; TCP 64 MiB both ways, sha256 | PASS | `E/20-*.txt`, `E/21-*.txt` |
| skb shape (bpftrace at `gro_receive_skb`): every data skb has 52 linear bytes after the Ethernet header and one frag; `ip_summed` 1; GRO skbs up to 64K at `tcp_v4_rcv` | PASS | `E/21-hds-tcp-shape.txt` |
| 1..5 byte segments, timestamps on (linear 52) and off (linear 40; frames are not padded here, so they are split too) | PASS | `E/22-hds-misc.txt` |
| ARP, ping 56/1472/2000 (fragments) | PASS | `E/22-*` |
| UDP self-checking datagrams 1..8000 bytes, IPv4 and IPv6 | PASS, 0 bad, 0 lost | `E/22-*` |
| IPv6 ping and TCP both ways (linear 72) | PASS | `E/22-*` |
| MTU 9000: TCP both ways, 3 frags per packet | PASS | `E/22-*` |
| `ethtool -K rx off` (`ip_summed` 0, `rx_csum_none`), GRO off | PASS | `E/22-*` |
| XDP attach with split on, and split on with XDP attached: both refused | PASS | `E/22-*` |
| ifdown/ifup, ring resize, channel change with split on | PASS | `E/22-*` |
| Split off again: `napi_gro_frags` path, `CHECKSUM_COMPLETE` | PASS | `E/22-*` |
| VLAN id 100: ping, fragments, TCP both ways, small segments, UDP (linear 56) | PASS | `E/23-hds-vlan.txt` |
| Mode off, series driver: tcp-data-split "n/a", `-G ... on` refused with extack, `rx_csum_none`, frag path, bind refused | PASS | `E/50-*.txt` |
| Padded frames (spec case 7), HBO, IPv4 options, IPv6 extension headers on the wire | NOT RUN in a guest (unit test only) | |

### Devmem RX (step 4)

Receiver rx-1: `ethtool -L combined 1`, tcp-data-split on,
`ncdevmem -n -l -s 192.168.100.11 -p 5201 -f rocm-ernic0 -t 0 -q 1 -v 7 -L`.

| Case | Result | Evidence |
| --- | --- | --- |
| First light: plain TCP 1 MiB: 726 `SCM_DEVMEM_DMABUF`, 0 `SCM_DEVMEM_LINEAR`, 0 validation failures, exit 0; all 726 data skbs `unreadable`, linear 52, `ip_summed` 1 | PASS | `E/30-first-light.txt`, `E/30-first-light-ncdevmem.err` |
| `page-pool-get` shows the dmabuf on the payload pool, `queue-get` on the RX queue | PASS | `E/31-page-pool-get.txt` |
| Plain TCP 256 MiB (185786 cmsgs, 0 linear) | PASS | `E/32-*.txt` |
| devmem TX (rx-2 `ncdevmem` client) -> devmem RX: 70000 bytes in 10000 sends, and 700000 in 100000 | PASS, 0 linear | `E/33-*.txt`, `E/60-final-end-to-end.txt` |
| Closing run on fresh boots: 64 MiB plain, TX->RX, small-write sender; 0 KASAN, 0 lockdep, 0 stalled pool | PASS | `E/60-final-end-to-end.txt` |
| Kill -9 ncdevmem mid-stream x3, then plain TCP, then bind again | PASS | `E/40-teardown-kill.txt` |
| ifdown/ifup with a binding, then send (binding follows to the new pool) | PASS | `E/41-teardown-ops.txt` |
| With a binding: split off and XDP refused by the core; ring 1024/4096, MTU 9000/1500, channels 1 -> 2; then send | PASS | `E/41-*` |
| With a binding on queue 0: channels 2 -> 1 | refused by the core's own check (`<=` in `ethtool_check_max_channel`); not a driver matter | `E/41-*` |
| Bind while down: `-ENETDOWN` | PASS | `E/41-*` |
| `ethtool -K rx off` with a binding: payload dropped (`rx_unreadable_drop`), nothing delivered | PASS (as designed) | `E/41-*` |
| ifdown/ifup mid-stream | connection reset; ncdevmem exits 0 with validated data; pools released. A plain TCP stream on the **stock** driver is reset the same way; cause not found | `E/41-*`, `E/42-*.txt` |
| Two queues, queue 1 bound, source port per T3: devmem 16 MiB on queue 1, hashed TCP on queue 0 during and after; a flow hashing to queue 0 is rejected by ncdevmem | PASS, lockdep armed | `E/44-teardown-more-rerun-lockdep.txt` |
| PCI function reset mid-stream with a binding: stream continues, 1.5 GB validated | PASS | `E/47-pci-reset-midstream.txt` |
| rmmod `ionic_rdma` + `ionic` mid-stream with a binding: both return, no splat from ionic; after ncdevmem exits 0 DMA entries and 0 pools remain | PASS (see finding C1) | `E/44-*rerun*`, `E/46-*.txt` |
| "stalled pool shutdown" / `page_pool_release_retry` in any boot | none | `E/45-*.txt`, `E/60-*` |
| Devmem RX over IPv6 or a VLAN, at MTU 9000, on rx-2 as receiver; stock `ncdevmem` without `-n`; `devmem.py`; queue-op failure paths | NOT RUN | |

`E/44-teardown-more.txt` is the first run of the last three rows: same results, but lockdep
had been switched off earlier by a KASAN report (bug P1), so it does not count for lockdep.

## 4. Bugs found

Fixed:

1. **Driver, patch 1: the module did not probe.** `register_netdevice()` refuses an ops-locked
   netdev with `ndo_set_rx_mode` ("ops-locked drivers should use ndo_set_rx_mode_async",
   `-EINVAL`). Converted to `ndo_set_rx_mode_async` (address snapshots synced with
   `__hw_addr_sync_dev()`, `ionic_lif_rx_mode()` called directly). Tree, stages s1..s7, messages
   and `patches-rx/mainline/` regenerated; `verify.sh` passes apart from the missing
   Signed-off-by; W=1 at s0..s7 and sparse at s1, s7 clean
   (`patches-rx/evidence/verify-testrx.log`, `build-testrx.log`; diff in
   `E/driver-fixes.diff`). The cover letter's testing paragraph now says what was run.

No other change to the driver, and none to the emulator or to `rx-interface.md`'s rules.

Pre-existing, not from the series:

- **P1. `ionic_reconfigure_queues()` leaks when the queue count grows into an index that needs
  a placeholder qcq** (e.g. `ifdown; ethtool -L 2; ifup; ethtool -L 4`): the loop that frees
  the temporary qcqs uses `qparam->nxqs` after it was swapped with `lif->nxqs`. Leaks 3 DMA
  allocations per queue and a page pool whose `p.napi` dangles; a later `page-pool-get` dump is
  a KASAN use-after-free in `page_pool_nl_fill()`, which also switches lockdep off. Reproduced
  on the unmodified v7.4-rc3 driver (`E/13-dma-leak-stock.txt`, `E/14-*.txt`). A one-line fix
  was tested in a variant module (`E/15-*.txt`) and is offered as
  `patches-rx/extra/0001-ionic-free-every-temporary-queue-after-a-queue-count-change.patch`;
  it is **not** in the tree or the series, and has no Fixes: tag.
- **P2.** ionic sets no DMA max segment size, so every bind logs DMA-API "mapping sg segment
  longer than device claims to support" when the core maps the udmabuf
  (`E/40-dma-api-sg-segment-warning.txt`). mlx5 calls `dma_set_max_seg_size()`; bnxt, fbnic and
  gve do not. Not changed.

Core behaviour seen:

- **C1.** After `rmmod` with a live binding the DMA-API reports pending mappings: they are the
  core's dma-buf attachment, held until ncdevmem closes its socket.
- **C2.** A TCP stream is reset by ifdown/ifup on the receiver, stock driver included.

Other observations: emulator `rx_queue_empty` tail drops under bulk TCP with a 1024 ring (TCP
recovers; not investigated); non-TCP frames over 256 bytes reaching a bound queue are dropped
as `rx_unreadable_drop` (spec D5).

## 5. What remains

- Not run: a firmware that really stops (heartbeat path, commands timing out in recovery) and
  a link flap reported by the device: the emulator can do neither, and the knobs only run the
  driver's handlers against a live device. Live padded/HBO/IP-option/extension-header frames
  (emulator unit tests only). A dma-buf drained by an application that holds its tokens. The
  stock selftest (needs flow steering). rx-2 as devmem receiver. XDP rounds in mode off were
  not repeated in the review round (they ran in the first run).
- Known and left: a failed unbind (start fails once) leaves the old queue on its dma-buf pool
  with the core's `WARN_ON` until the next queue rebuild (core behaviour for any driver);
  after a double start failure the queues stay down until `ifdown/ifup` (as after a failed
  `ionic_start_queues_reconfig()` upstream); with two queues on one dma-buf the second
  stays without buffers while the first keeps recycling its own (page pool behaviour; the
  retry timer polls at 100 Hz meanwhile).
- Decide whether P1's fix and a `dma_set_max_seg_size()` patch go upstream ahead of the series.
- `BIT_ULL(17)` has not been checked against shipped firmware (stated in patch 2 and the
  cover letter; no extra gate was added).
- The ifdown reset (C2) is unexplained.
- Emulator: no fault site for `Q_INIT`/`Q_CONTROL`, no heartbeat-stop, no link-down knob.

## 6. Questions only real firmware can answer

1. Does any ionic firmware split headers, and how is it negotiated (queue feature bit, LIF
   feature, queue version, a header-buffer size or ring in `Q_INIT`)?
2. Where is the header length reported, does a 2X completion coexist with `hwstamp`, and does
   `comp_sz` in the identity already report the doubled size?
3. What is split: UDP, IPv6 extension headers, tunnels, padded frames, pure ACKs, bad-checksum
   frames; is there a threshold; what happens when headers exceed buffer 0?
4. Checksum: is `CALC` set on every frame, what does `csum` cover and in which byte order on
   real devices, do `_OK` flags accompany it, and what do they mean for tunnels?
5. Can queues of one LIF differ in features, can one RX queue be re-initialised while the LIF
   is up, and is `Q_CONTROL(DISABLE)` synchronous (needed to replace the queue-op stub)?
6. Can the device DMA to arbitrary guest-physical dma-buf pages, with which limits on SG
   element count and length, and do RX CMB rings or timestamps exclude header split?
7. Flow steering: n-tuple filters and RSS contexts, so the stock selftest can run without `-n`.
8. Does real firmware pad or strip short frames, and does it deliver frames while a queue is
   being restarted?

## Review round

2026-11-18/19. Findings of the independent reviewers, each checked against the code. The driver
series is now **8 patches** (new patch 7, "ionic: keep an Rx queue going when its buffers run
out"; the queue ops are patch 8): `patches-rx/mainline/000[0-8]-*.patch`, stages `s0..s8`,
`linux-rx` = `s8`, module `rx5`. `verify.sh` and W=1 **and sparse at every stage** pass
(`patches-rx/evidence/verify-review.log`, `build-review.log`). Emulator: `rocm-ernic-rx`
rebuilt as `test-rx/ernic/rx-hds-2`, stack 2 restarted on it. Test-only driver knobs (not in
the series): `patches-rx/extra/test-only-ionic-fault-knobs.diff`, modules `rx5fi`, `rx3fi`.
Evidence: `test-rx/evidence/review/` (`R..` below), emulator checks in
`test/evidence-emu-rx/review/`.

| # | Finding | Verdict | What was done, and the evidence |
| --- | --- | --- | --- |
| 1 | Emulator: an HDS queue reads the SG list of a splittable frame that would fit `buf0`; only the appendix said so | FIXED (words) | Behaviour kept (drop, `rx_desc_fetch_error`; the existing unit test asserts it). Now stated in `docs/ionic.rst` "What is split", the `eth_rx_bufs()` comment and `rx-interface.md` P5 proper. doc8, Sphinx (only the old `api.rst` warning), codespell pass. |
| 2 | Emulator: `Q_INIT` refusal always logs the features clause | FIXED | Features are named only where they are read (Rx queue, mode on). New unit test rows failed first (2 FAIL), pass after; GCC 13 Debug ctest (same 3 host-specific failures as before), GCC Release, Clang 18, ASan 62/62, clang-format, clang-tidy 23 on the 6 files that include the changed fake. |
| 3, 9 | Driver: failed `ndo_queue_start()` leaves the netdev detached; FW reset in mid-op | FIXED (patch 8) | Attach on both branches; with `IONIC_LIF_F_FW_RESET` set the start keeps the new rings, frees the old and returns 0. Before (`rx3fi`): after a double failure `ethtool` and `ifup` return `-ENODEV` (`R19`). After: netdev present, `ifdown/ifup` recovers, bind works (`R11`); single failure at bind (`R10`) and at unbind (`R12`, core `WARN_ON` as predicted); FW reset begun between stop and start of a bind and of an unbind: queue ends on the right provider, 16 MiB validated (`R16`). |
| 4 | Driver: unbind in the window where `ionic_restart_lif()` had dropped the lock with FW_RESET still set | FIXED (patch 8) | The flag is cleared before the instance lock is dropped. By reading; the window itself was not reproduced. Related runs: unbind and bind attempts while the FW is held down, then FW up: ordinary pools, no WARN (`R15`). |
| 5 | Driver: shadow qcq freed while the old pool's `p.napi` points into it | FIXED (patch 8) | `ionic_swap_queues()` gives the pools that go to the temporary queue no NAPI (also covers ring/channel changes). Not reproducible as a race; side effect seen: page-pool dumps over pools leaked by P1 no longer trip KASAN (`R32`, `R33`: 0 KASAN with leaked pools present). |
| 6 | Driver: Tx timeout work tests FW_RESET before taking the lock | FIXED (patch 1) | Both tests are under the instance lock. Work run idle, under TCP, around a devmem stream and while the FW was down (`R17`, `R15`). `impl-driver-rx.md` 4.3 corrected. |
| 7 | Lockdep run covers less than `LOG.md` said | FIXED | `LOG.md` reworded. `opslock.sh` has split-interrupt rounds and pins its XDP program (`R22`, `R23`: PASS on both guests, HDS off and on; `R21` mode off, where the split-interrupt and XDP steps did not take effect). New `guest/stress.sh`: 134 binds against 242 ring/channel/MTU/ifdown/PCI-reset operations and dumps in 90 s, no splat (`R35`). FW down/up, Tx timeout and link down ran through test-only driver knobs (`R13`..`R18`), the firmware alive throughout. |
| 7b | ... emulator heartbeat-stop and link-down knobs | DEFERRED | Not built; a really dead firmware (command timeouts in recovery) is still NOT RUN. |
| 8 | Driver: Rx queue on a dma-buf smaller than the ring goes deaf | FIXED (new patch 7) | Reproduced on `rx3` (`R01`: ping 0/5 both ways, `rx_0_alloc_err` 0; `R03`). `ionic_rx_fill()` now rings the doorbell for what it posted, counts `rx_N_alloc_err`, and a timer reschedules NAPI when the ring is empty. After: ring 16384 ping 5/5 and 16 MiB validated (`R05`), MTU 9000 ring 8192 (`R06`), two queues on one dma-buf: 256 MiB validated on queue 0 (`R04`; queue 1 never gets buffers there, see section 5), empty ring refilled 0.3 s after buffers appear with no Tx in between (`R07`). The limit is stated in patch 8 and the cover letter. |
| 10 | Cover letter testing paragraph incomplete | FIXED | Rewritten: what ran, the knobs, the DMA-API warning, the existing reconfigure leak and its KASAN report, an explicit "Not tested" list. Devmem RX over IPv6 and a VLAN, with split interrupts and at MTU 9000 were run instead of listed (`R34`, `R06`). |
| 11 | `BIT_ULL(17)` unchecked against firmware | FIXED (words) | Stated in patch 2's message and the cover letter. No module parameter or extra gate: REJECTED as not worth a second invented interface in a PoC. |
| 12 | `tcp-data-split off` accepted on a device without header split | FIXED (patch 6) | Any explicit setting is refused there (`-EOPNOTSUPP`, extack); `auto` and plain ring changes still work (`R20`, mode off). |
| 13 | HDS queue accepts frames up to MTU + 18 + 256 | FIXED (words) | Said in patch 5's message; no drop added. |
| 14 | Patch 3 message wrong about fbnic | FIXED | Reworded (checked against `fbnic_rx_csum()`). |
| 15 | Cover: sparse only at s1/s7, base-commit without prerequisites, missing blank line | FIXED | Sparse rerun at s0..s8; `prerequisite-patch-id` lines for the six TX patches; blank line. checkpatch now reports 6 errors on the cover letter, one per `prerequisite-patch-id` line (it wants a commit description for any 40-digit hash; `git format-patch --base` emits the same lines). `verify.sh` runs checkpatch on the cover letter without a `GIT_DIR`: `linux-rx` is a partial clone and git went to the remote for each unknown hash, for minutes (I let it run that way three times before finding it; those lookups reached the remote read-only). |
| 16 | Comment "only ever holds a few pages" | FIXED | "one page for every 16 descriptors". |

Regression after the changes, driver `rx5`, mode on: closing run (64 MiB plain, TX -> RX
100000 sends, small writes; `R30`), kill x3 (`R31`), teardown with a binding (`R32`; its XDP
line failed for a missing pinned program and was redone in `R34`), two queues, PCI reset
mid-stream, rmmod with a binding (`R33`), the header split cases with ordinary pages (`R36`,
same PASS list as `E/22`), final state (`R40`: 64 MiB validated, both guests 0 KASAN, 0
lockdep reports, 0 stalled pools this boot): same results as the first run, lockdep armed, no
KASAN. The only reports are the known ones (DMA-API sg segment at bind; pending DMA mappings
at rmmod with a live binding, C1; the injected-failure WARNs of `R11`, `R12`, `R19`).

Not done in this round: TSan/valgrind/coverage for the emulator change (a log line and a test
fake; no shared state or wire data touched); `git am`; a full kernel build.


## XDP frag offset fix (2026-11-19, `rx-xdp-frag-fix.md`)

Supersedes "Driver now loaded" in section 1 and two "Not run" items of section 5.

- The series is regenerated on v7.4-rc3 + the `net` XDP frag offset fix + the TX series; patch
  4 uses `bi->page_offset` for the sync and the frag fill. Module **`rx6`** is loaded in both
  guests (`ionic-mod.sh load rx6` after a reboot); `rx5` has the bug.
- Multi-buffer XDP echo (MTU 9000, 8000-byte datagrams, tcp-data-split off): `rx5` about half
  the echoes corrupt in their last 426 bytes with XDP_TX and XDP_REDIRECT; `rx6` 10000/10000
  intact with each, mode on and mode off.
- Section 5, now run and passing on `rx6`: rx-2 as devmem receiver; the XDP rounds in mode off.
- Regression on `rx6` (header split with pages, first light 64 MiB, devmem TX -> RX): PASS,
  0 KASAN, lockdep armed.
