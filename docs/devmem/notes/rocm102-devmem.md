# Stack 3 (`ionic-devmem-r102`): the devmem work on the upstream ROCm 10.2 stack

2026-11-19 (evidence stamped in UTC, 2026-11-19T23:45Z to 2026-11-20T01:15Z). Everything here
was RUN unless marked **NOT RUN**. `B` = `/var/tmp/ionic-devmem/test-rocm102`, `Q` =
`/var/tmp/ionic-devmem/qemu-minimal-2.1` (v2.1.0), `R` = the `git archive 2d3a3d02` snapshot
`B/ernic/poc/src` of rocm-ernic `feat/devmem-poc`. Baseline this is compared with:
`notes/rocm102-baseline.md`. Nothing in rocm-ernic, qemu-minimal or stacks 1 and 2 was changed.

**Result.** Both changes work on the ROCm 10.2 stack and neither broke what the stack does.

- **A (server from 2d3a3d02):** the guests' unmodified ionic now shows scatter-gather, Tx
  checksum and TSO on; every upstream test passes with the same verdicts as the baseline, SRQ
  passes, plain TCP hashes match, no emulator error counter moved.
- **B (ionic = srq 0001-0011 + p4 + devmem-tx, `ernic_p4.ko` rebuilt):** `max_srq` 256, SRQ and
  P4 pass, every upstream test passes again; **devmem TX binds and the whole matrix passes**:
  35 cases, 0 mismatching bytes at the receiver anywhere, 74.1 GiB verified in the 360 s run with
  79571 of 79571 completions.
- **No product bug** was found in the emulator or the driver series. Findings (section 5) are a
  counter signature on P4-enabled servers, two things an image bake must do, one upstream
  qemu-minimal staging rule that the new srq numbering has outgrown, and harness lessons.
- No rate moved beyond this host's noise in a way that follows the server or the driver
  (section 4).

## 1. State the stack is left in

**Running**, compose project `ionic-devmem-r102`, cold-cycled at 01:09Z, then:

| Item | State |
| --- | --- |
| NIC servers `ernic-1`, `ernic-2` | `B/ernic/poc-rel/build/rocm-ernic` (2d3a3d02, **Release**, sha256 `d50b1d29...`) bind-mounted read-only over `/usr/local/bin/rocm-ernic` by `docker-compose.override.yml`; arguments unchanged (`--p4 --p4-guest-scope load -T tap0`), `nic_profile : compat` (header split off). Persistent across `stack.sh cycle`; `stack.sh down; stack.sh server image; stack.sh up` returns to the image's binary |
| Guest ionic, both guests | **Loaded with `insmod`, not installed**: `~/ionic-mod/devmem/mod/ionic.ko` (srcversion `08D37B49C509F37A8E58798`, build-id `412941e1...`), `ionic_rdma.ko` (build-id `8cfe655d...`), `~/ionic-mod/devmem/p4/ernic_p4.ko` (build-id `757781e4...`). **A guest reboot or `stack.sh cycle` returns to the baked DKMS modules** (ionic `44164456...`); reload with `B/scripts/ionic-mod.sh load devmem` (20 s) |
| Guest configuration | one `quick-start test` (base) run after the load: amdgpu loaded, `rocm-ernic0` = 10.102.100.11 / .12 only, MTU 1500, rings 1024/1024, Tx offloads at their defaults (all on), `tcp_retries2` 15 |
| NIC 1 | **no P4 program** (cycled after the last P4 run) |
| Left on guest disks | `~/ionic-mod/{baked-check,devmem}` (sources + built modules), `/usr/local/bin/devmem_tx`, `/usr/local/share/devmem-tx/probe.bt`, `~/devmem/{devmem_tx,ncdevmem}` |
| GID table | r102-2 has the link-local GID twice (index 0 and 1) and the IPv4 GID at index **2**; r102-1 has IPv4 at index 1 (F1) |
| Kernel logs | no BUG/WARNING/Oops/Call Trace in either guest; 0 dma-buf objects |
| Last checks in this state | `quick-start test` PASS (`B/evidence/quickstart/final-base/`), devmem smoke PASS (`B/evidence/final/devmem-smoke.txt`), SRQ PASS (`B/evidence/srq/final.txt`), TCP sha256 both ways PASS, loaded = built modules (`B/evidence/final/verify.txt`), facts `B/evidence/facts/30-final-devmem-*` |

Stacks 1 (`ionic-devmem`) and 2 (`ionic-devmem-rx`) were not touched.

## 2. Change A: the emulator

### 2.1 What was done

    B/scripts/build-ernic.sh poc 2d3a3d02                       # Debug (built, NOT RUN in the stack)
    BUILD_TYPE=Release B/scripts/build-ernic.sh poc-rel 2d3a3d02   # the one swapped in
    BUILD_TYPE=Release B/scripts/build-ernic.sh base-rel bd902345  # control: branch base, no devmem commits
    B/scripts/stack.sh down; B/scripts/stack.sh server poc-rel; B/scripts/stack.sh up

- `build-ernic.sh` is the adapted copy: `git archive REF` into `B/ernic/LABEL/src`, built inside
  the stack's own `...-ernic.90434a2-vfu.3719c3a` image (`ldd`: `/usr/local/lib/x86_64-linux-gnu/libvfio-user.so.0`).
- **Release, not Debug**, because the image's binary is a Release build (no debug sections,
  `_FORTIFY_SOURCE` `_chk` imports), so rates compare like with like.
- **The image's server is 47 commits behind the branch**, not 4: `90434a2..2d3a3d02` includes
  the GLib removal and the CMB fix as well as the four devmem commits. `base-rel` (bd902345)
  exists to tell the two apart; it was needed only for the rate comparison.
- `stack.sh server LABEL|image` rewrites the two `ernic-N` lines of the override; the stack must
  be down. Evidence: `B/evidence/A-emulator/server-N.txt` (sha256, command line, profile,
  `rocjitsu-vram` still mapped).

### 2.2 Offloads in the guests (unmodified baked ionic `44164456...`)

`B/evidence/A-emulator/guest-N-ethtool.txt`, identical on both guests:

| Feature | Baseline | A |
| --- | --- | --- |
| `tx-scatter-gather` | off [fixed] | **on** |
| `tx-checksum-ip-generic` | off [fixed] | **on** |
| `tx-tcp-segmentation`, `tx-tcp-ecn-segmentation`, `tx-tcp6-segmentation` | off [fixed] | **on** |
| `generic-segmentation-offload` | off [requested on] | on |
| rings | 1024/1024 | 1024/1024 |
| `max_srq` | 256 | 256 |

### 2.3 Upstream tests after A (`B/scripts/qs-all.sh A`; `B/evidence/quickstart/A-*/`, summary `A-results.txt`)

| Test | Baseline | A | Verdict |
| --- | --- | --- | --- |
| base (GPU, RDMA, NVMe) | PASS, `ib_send_bw` 1950 | PASS x3 (`A-base-first`, `A-base`, `A2-base`): 1363, 1903, 2156 | same |
| `--sdxi-test` | PASS, 4 MiB identical x2 | PASS, 4 MiB identical x2 | same |
| `--hipfile-test` | PASS, 205.7 / 210.2 MB/s, fio 212.4 / 214.4 | PASS, 199.6 / 205.1, fio 210.4 / 209.7 | same |
| `--rocm-rdma-test` host-alloc | PASS, 1158 MiB/s, 58720256 bytes validated | PASS, 1987 MiB/s, 58720256 validated | same |
| `--rocm-rdma-mode dmabuf` | PASS, 1305 MiB/s, validated | PASS, 1816 MiB/s, validated | same |
| `--rocm-rdma-mode vram` | expected FAIL, `error=14` | expected FAIL, `error=14` | same |
| `--p4-test` | PASS, 170.5 MB/s | PASS, 149.6 MB/s (10 more samples: 171.4 to 176.7) | same; the 149.6 is one sample, section 4 |
| SRQ (`srq-test.sh`, not in the baseline) | n/a | PASS before and after P4: `ib_send_bw --use-srq` 1903 / 2018 MiB/s, `ibv_srq_pingpong` 4 QPs x 2000 | new reference |
| TCP 64 MiB each way, sha256 at the receiver | PASS | PASS before and after (0.27 to 0.37 s per 64 MiB with TSO) | same |
| `ib_send_bw` across the runs | 1134 to 1950 | 687 to 2156 (687 in `A-hipfile`) | section 4 |
| NVMe fio | write 20, read 1528 to 2049 | write 20 to 21, read 1482 to 1972 | same |

After the whole sequence: emulator `tx_dma_error`, `tx_desc_data_error`, `tx_desc_fetch_error`,
`rx_queue_empty`, `frames_rx_dropped` all 0 on both servers; guests `hw_rx_dropped` 0,
`TcpRetransSegs` 0, `tx_tso` 2104 / 2111 (TSO was used); no kernel log splat
(`B/evidence/A-emulator/ernic-N-stats-final.txt`, `B/evidence/quickstart/A-dmesg-r102-N.txt`).
The Rx overrun recorded in `notes/test-emulator-offloads.md` did **not** occur in these tests;
it did in the 360 s devmem run (F3).

## 3. Change B: the driver

### 3.1 What was built, and how it was shown to be the right thing

    B/scripts/ionic-mod.sh stage baked-check srq:9 p4      # control: what the image baked
    B/scripts/ionic-mod.sh stage devmem srq p4 devmem-tx   # srq 0001-0011, p4 0001, devmem-tx 0001-0007
    B/scripts/ionic-mod.sh build baked-check; B/scripts/ionic-mod.sh build devmem
    B/scripts/ionic-mod.sh load devmem                     # rmmod ernic_p4 ionic_rdma ionic; insmod the three; verify

- **Procedure: out-of-tree `make M=` in each guest, `insmod`, nothing persistent** (the least
  invasive one of the study). Sources are staged on the host from v7.2.6
  (`/var/tmp/ionic-devmem/linux-7.2.6`) with `git apply` in a plain directory, patches from the
  2d3a3d02 archive, in the image's order; the guest's own `/usr/src/ionic-ernic-7.2.6/Kbuild` is
  used, and `ernic_p4.ko` is built from the guest's own `/usr/src/ernic-p4-0.2.0` with
  `IONIC_SRC=` the patched tree and `IONIC_SYMVERS=` the `Module.symvers` just built.
  Patch list and hashes: `B/ionic-mod/devmem/SOURCE`.
- **"Exactly as baked" was checked twice** (`B/evidence/B-driver/`):
  - `baked-source-check.txt`: v7.2.6 + srq 0001-0009 + p4 is byte-identical to the guest's
    `/usr/src/ionic-ernic-7.2.6/{eth,rdma,uapi}`; the devmem tree differs from it in
    `ionic_dev.h`, `ionic_lif.[ch]`, `ionic_stats.c`, `ionic_txrx.c` only (plus srq 0010/0011's files).
  - `build.txt`: the `baked-check` build in the guest gives `ionic.ko` srcversion
    `44164456D34C01FB438D570`, the baked module's. So the build procedure reproduces the image's.
- **Loaded modules are the rebuilt ones** (`load.txt`, `verify-after-load.txt`,
  `verify-after-regression.txt`, `B/evidence/devmem-tx/verify-after-matrix.txt`,
  `B/evidence/final/verify.txt`): srcversion and `.note.gnu.build-id` of `/sys/module/*` equal the
  built files on both guests every time.

| Module | Baked: srcversion / build-id | Loaded: srcversion / build-id |
| --- | --- | --- |
| ionic | `44164456D34C01FB438D570` / `c2700de0...` | `08D37B49C509F37A8E58798` / `412941e1...` |
| ionic_rdma | `24E57766CB83B79E273C7B0` / `f19725ea...` | `24E57766CB83B79E273C7B0` / `8cfe655d...` |
| ernic_p4 | `66D2E2195859E80AE848D07` / `a00cb8b3...` | `66D2E2195859E80AE848D07` / `757781e4...` |

  **srcversion does not change for `ionic_rdma` and `ernic_p4`** although they were recompiled
  against changed ionic structures; only the build-id tells them apart (F5).
- **The baked `ernic_p4.ko` does not load beside the devmem ionic**
  (`baked-p4-vs-devmem-ionic.txt`): `ernic_p4: disagrees about version of symbol
  ionic_adminq_post_wait` (CRC `0x2a8cdd9f` baked, `0x74a6046e` now). The rebuilt one loads and
  `/dev/ernic-p4` appears. This settles the study's open question: it must be rebuilt.
- After the load: Tx offloads on, `tx_unreadable_drop` and four `tx_N_unreadable_drop` in
  `ethtool -S`, `max_srq 256`, port ACTIVE (`after-load-facts.txt`).

### 3.2 Upstream tests after B (`qs-all.sh B`; `B/evidence/quickstart/B-*/`, `B-results.txt`)

| Test | Baseline | A | B | Verdict |
| --- | --- | --- | --- | --- |
| `ibv_devinfo` `max_srq` | 256 | 256 | 256 (`max_srq_wr` 65535, `max_srq_sge` 2) | same |
| base | PASS | PASS | PASS (also `B2-hipfile`, `final-base`) | same |
| `--sdxi-test` | PASS | PASS | PASS, 4 MiB identical x2 | same |
| `--hipfile-test` | 205.7 / 210.2, fio 212.4 / 214.4 | 199.6 / 205.1, fio 210.4 / 209.7 | 215.8 / 208.4, fio 263.2 / 264.2; rerun `B2-hipfile`: 233.8 / 228.3, fio 236.3 / 237.1 | PASS; fio rate differs, section 4 |
| `--rocm-rdma-test` host-alloc | 1158 MiB/s | 1987 | 1535, 58720256 bytes validated | same |
| `--rocm-rdma-mode dmabuf` | 1305 | 1816 | 1612, validated, dma-buf MR on both sides | same |
| `--rocm-rdma-mode vram` | expected FAIL | expected FAIL | expected FAIL, `error=14` | same |
| `--p4-test` (rebuilt `ernic_p4.ko`) | 170.5 MB/s | 149.6 | 173.3 | same |
| SRQ | n/a | 1903 / 2018 | PASS before and after P4: 1165 / 880 MiB/s; pingpong 2000 round trips | PASS; rate in section 4 |
| TCP sha256 both ways | PASS | PASS | PASS before and after | same |
| `ib_send_bw` in the runs | 1134 to 1950 | 687 to 2156 | 731 to 1396 | section 4 |

Emulator error and drop counters all 0 after the sequence; no kernel log splat
(`B/evidence/B-driver/ernic-N-stats-after-regression.txt`, `B/evidence/quickstart/B-dmesg-r102-N.txt`).
The RDMA dma-buf path (amdgpu 7.2.3 exports, ionic_rdma registers) and the devmem TX path were
both used in one boot without trouble, which the study had left open.

### 3.3 devmem TX matrix (udmabuf, r102-1 -> r102-2)

Tools: `B/bin/devmem_tx` (sha256 `1579ebae...`, the stack 1 binary, unchanged) installed as
`/usr/local/bin/devmem_tx`; harness `B/scripts/devmem-tx/` (adapted copy: addresses, no
`/mnt/hostfs`, emulator stats through the shim, `enp0s6`, DMA-API checks reported N/A);
`run-matrix.sh` runs the rows. One evidence file per row in `B/evidence/devmem-tx/`, one line per
case in `SUMMARY.txt`, captures in `pcap/`. The receiver recomputes the expected stream from
the plan and compares every byte; it knows nothing of what the sender believes it sent.

| Row | What | Result | Evidence |
| --- | --- | --- | --- |
| bind | `devmem_tx` bind-tx: `bind-tx ok: ifindex=4 dmabuf_id=1`; `ncdevmem -f rocm-ernic0 -s .12 -t 0 -q 1`: `got tx dmabuf id=2`, `tx ok`, receiver sha256 of the 46 bytes equals the sender file's (`b127ee0a...`). On the baseline both got `EOPNOTSUPP` | **PASS** | `M2-smoke-bind.txt`, `M2-ncdevmem-and-controls.txt` |
| controls | `-m copy`, `-m zc`, 16 MiB each, 0 mismatches | PASS | `M2-ncdevmem-and-controls.txt` (its "CONNECT-FAILED" line is the strace step, which has no receiver by design) |
| M3 TSO off | GSO on, then GSO off too: 128 MiB page-aligned and 57 MiB unaligned each. 92801 / 41679 unreadable non-GSO skbs | PASS x4, 0 mismatches, 0 retransmits | `M3-tso-off.txt` |
| M4 TSO on | same two streams: 2256 / 992 unreadable GSO skbs, up to 17 frags | PASS x2, 0 mismatches, 0 retransmits | `M4-tso-on.txt` |
| M6 offsets and frags | 300 x 1 byte at odd offsets; 2 bytes across a page boundary; 100003-byte sends from offset 123; 50 iovecs of 5003 with gaps; last 4095 bytes of the dmabuf; 480 iovecs of 128 bytes, TSO on and off; **15 frags, non-GSO, the exact limit**; 1024 iovecs of 1400 | PASS x9, 0 mismatches, `tx_unreadable_drop` +0, linearize +0. Two cases show one tail-loss-probe retransmit each (`TCPLossProbes` +1, no drop anywhere), as stack 1 did | `M6-positive.txt` |
| M6 16/17 frags | one-byte sends back to back (16-frag skb forms after 43 bytes), 16 frags non-GSO, 17 frags of 16 bytes (twice) | **clean counted drop**: `tx_unreadable_drop` +9, +9, +10, +8 = every transmission attempt of such an skb; netdev `tx_dropped` +0, emulator errors 0, receiver got 0 bytes (43 verified bytes in the first), no log line; binding and dma-buf released when the orphan times out (75 to 78 s); **the next positive run passes** (50 MB, 0 mismatches) | `M6-negative.txt` |
| M7 tx off | `ethtool -K tx off` before connect (100 KB and 1 KB sends) and 3 s into a stream | receiver: 0 bytes, 0 bytes, 13.5 MB verified prefix then nothing. **No zero-filled payload, 0 mismatches** | `M7-tx.txt` |
| M7 sg off | same with `sg off` | 0, 0, 14.8 MB verified prefix; no zero-filled payload | `M7-sg.txt` |
| M7 both | same with `tx off sg off` | 0, 0, 14.8 MB verified prefix; `tx_unreadable_drop` +10 / +72 (patch 0006's no-frags skbs) | `M7-both.txt` |
| M7 captures | 18 capture summaries (emulator 1's `tap0` and the receiver guest, 9 cases): **all-zero payloads: 0 in all 18; bad TCP checksum 0 in all 9 on `tap0`**. The receiver-guest captures of the three stream cases show "bad" checksums on GRO-merged frames (longest frame 65226), the same artefact as on stack 1 | PASS | `pcap/M7-*.pcap` |
| M9 teardown | 64 x 1 MiB queued, then `-X close-sock`, `unbind`, `free-dmabuf`, `exit` with sends in flight | receiver verified all 67108864 bytes in all four; bind/unbind/free 1/1/1 (first case frees 3: the two orphans left by M7); 0 dma-buf objects after | `M9-close-unbind-free-exit.txt` |
| M10 sustained | 360 s, 1000003 bytes per send in 4 iovecs of 250001 with gaps, offsets covering all 4096 pages, TSO on | **79571238713 bytes (74.1 GiB) verified, 0 mismatches; 79571 sends, 79571 completions, 0 copied, 0 gaps, 0 errors**; 210.8 MiB/s; 399 retransmits = receiver `hw_rx_dropped` 399 (F3) | `M10-sustained.txt` |

After the matrix: no BUG/WARNING/Oops/Call Trace in either guest
(`dmesg-r102-N-after-matrix.txt`), emulator `tx_dma_error` / `tx_desc_data_error` /
`tx_desc_fetch_error` 0 in every case, modules still the rebuilt ones. One unrelated line in
r102-1 during M10: `clocksource: Watchdog remote CPU 5 read timed out` (a vCPU descheduled on
this loaded host).

**How it was shown the skbs were unreadable devmem skbs.** The guest kernel has kprobes and
vmlinux BTF; the rebuilt modules have no BTF (no pahole in the image), so `probe.bt` uses
kprobes with vmlinux types, as on stack 1 (bpftrace is in the image). Per case:

- `kprobe:ionic_start_xmit`: skbs with `skb->unreadable` set, their `data_len` summed, `nr_frags`,
  bit 0 of `frags[0].netmem` (net_iov). In every positive case unreadable payload bytes >= the
  bytes the receiver verified (equal when there was no retransmit) and `frag0_net_iov` is 1 for
  every unreadable skb with frags; readable skbs are the handshake and FIN only (3 to 5 skbs,
  206 to 314 bytes; 25 skbs / 2918 bytes in the 360 s run).
- `kretprobe:ionic_tx_map_frag.constprop.0` inside `ionic_tx_map_skb` of an unreadable skb: the
  returned DMA address, by destination port, against the dmabuf's PFNs from `/proc/self/pagemap`:
  **`outside-the-dmabuf=0` in every case** (M10: all 4096 pages of the dmabuf seen, none outside).
  The guest has no IOMMU, so DMA address = physical address.
- `dma_map_page_attrs` / `dma_map_phys` in that window: one call per unreadable skb (the header),
  none for frags (110 skbs, 110 calls, 1130 frag maps in the smoke run).
- Completions: `copied=0` everywhere.
- **Not available here:** `CONFIG_DMA_API_DEBUG` (the kernel lacks it), so the unmap-side check
  stacks 1 and 2 had is N/A; `summarize.py`'s `dma_api_other=0` column means nothing on this stack.

**NOT RUN** (devmem): `rmmod ionic` with a live TX binding (by instruction; known core
use-after-free on v7.2.x) and the other rmmod row; MTU 9000 / 9198 rows (this stack's tap and
bridge stay at 1500); M8 (route through another netdev); M9 link down/up and ring/channel/MTU
reconfiguration mid-stream; the frag-size threshold row; the no-0006 control module; XDP rows;
IPv6; devmem TX from an amdgpu buffer (`devmem_tx_gpu`; on 7.2.6 it would read GTT pages, not
VRAM); the header-split profile; the Debug server build in the stack; a DKMS-persistent install.

## 4. Rates: what moved and why it is not attributed to A or B

One `quick-start test` gives one `ib_send_bw` sample of about 30 ms (1000 x 64 KiB). On this
host that single sample is not a measurement: load average was 7 to 10 throughout, two of those
CPUs being **this stack's own `rocm-sdxi` servers, which spin at 99 % from start-up** (16 min of
CPU in 16.5 min of life, upstream images), others stacks 1 and 2.

`B/scripts/bench.sh` takes 20 back-to-back samples (and 10 of 30000 iterations, 20 with
`--use-srq`, 10 `p4_fp8_bench`); everything in `B/evidence/bench/`. Medians, MiB/s:

| Block | Server | ionic | `ib_send_bw` | x30000 | SRQ | P4 MB/s |
| --- | --- | --- | --- | --- | --- | --- |
| `ref-image-1`, fresh boot | image (90434a2) | baked | 1967 | n/a | 1941 | 171.4 |
| `ref-base-rel-1`, fresh | bd902345 | baked | 1706 | n/a | 1882 | 170.7 |
| `A-poc-rel-2-fresh` | 2d3a3d02 | baked | 1644 | n/a | 2050 | 173.4 |
| `A-poc-rel-3-configured` | 2d3a3d02 | baked | 1629 | 2092 | 1833 | 173.6 |
| `A-poc-rel-1`, after the A sequence | 2d3a3d02 | baked | 1186 | n/a | 1196 | 173.9 |
| `B-fresh-devmem-ionic-1` | 2d3a3d02 | devmem | 1375 | 2342 | 2162 | 174.6 |
| `B-fresh-devmem-ionic-2` (P4 program in NIC 1) | 2d3a3d02 | devmem | 2086 | 1792 | 1121 | 172.7 |
| same boot, alternating: `interleave-1` | 2d3a3d02 | baked | 1194 | 1383 | 1551 | 173.7 |
| `interleave-2` | 2d3a3d02 | devmem | 1408 | 1207 | 1513 | 173.5 |
| next boot: `interleave-3` | 2d3a3d02 | devmem | 1344 | 1705 | 1594 | 173.4 |
| `interleave-4` | 2d3a3d02 | baked | 1084 | 1557 | 1233 | 173.6 |
| `interleave-5` | 2d3a3d02 | devmem | 2130 | 1749 | 1692 | 176.3 |

- **P4:** 170 to 177 MB/s in every block, whatever the server and driver. The 149.6 of `A-p4`
  is a single low sample. No change.
- **Driver (B):** alternating baked and devmem modules in one boot, the devmem module is never
  the slower of a neighbouring pair by more than the pairs differ among themselves. No change.
- **Server (A):** blocks of the same configuration differ by up to 1.8x (1186 to 1644 to 1629
  for 2d3a3d02 with the baked ionic; 1375 to 2130 with the devmem ionic), which is more than the
  1967 / 1706 / 1644 spread between the three servers on fresh boots. The RDMA send path is not
  touched by the Tx-offload commits. **Not shown to be a change; not excluded either:** the one
  image-server block was the highest of the fresh-boot three. A decisive answer needs an idle
  host and the servers alternated over several boots. **NOT RUN.**
- **`ib_send_bw` inside the quick-start runs:** baseline 1134 to 1950 (8 samples), A 687 to
  2156 (9), B 731 to 1427 (9). B's are lower, but the same-boot alternation above says the
  driver is not the cause; the B sequence ran while the host load average was 9 to 10 against
  7 to 8.5 for A's.
- **hipFile fio:** 212 / 214 (baseline), 210 / 210 (A), 263 / 264 (B), 236 / 237 (B rerun). The
  two guests run it at the same moment and move together; it is a 0.3 s read of 64 MiB on the
  NVMe -> GPU BAR path, which neither change touches. Run-to-run, and upward.
- **ROCm RDMA** 1158 / 1305 (baseline), 1987 / 1816 (A), 1535 / 1612 (B): single samples of the
  same kind as `ib_send_bw`.

## 5. Findings

None is a defect of the Tx-offload emulator change or of the devmem-tx series. Product fixes
were out of scope; nothing was edited or posted.

- **F1. The RoCE GID index of the fleet address is not stable across an ionic_rdma load
  (harness lesson; RDMA core ordering).** After the last `ionic-mod.sh load`, r102-2's table was
  `0: fe80::706f:63ff:fe6d:2`, `1: fe80::706f:63ff:fe6d:2` again, `2: ::ffff:10.102.100.12`;
  r102-1's was `0: fe80...`, `1: ::ffff:10.102.100.11`. My `ibv_srq_pingpong -g 1` then failed
  (`Failed to modify QP[0] to RTR`: server link-local, client IPv4) and the client sat until
  its 120 s timeout (`B/evidence/srq/final-FAILED-gid-index-1-is-link-local.txt`). Cause as far
  as traced: the netdev's link-local address event reached the GID cache before the default GID
  was set, so it became a second, address-derived entry at index 1; the earlier loads and all
  boots happened the other way round. perftest picks its index itself and was unaffected
  (`final-base` passed in that state). `srq-test.sh` now looks the index up per guest; rerun
  PASS (`B/evidence/srq/final.txt`). Not established: whether a plain boot can order it this way.
- **F2. Loading a rebuilt ionic triggers an autoload of the baked `ionic_rdma` (harmless
  here).** `insmod ionic.ko` creates the auxiliary devices, udev modprobes the installed
  `ionic_rdma`, and the kernel refuses it: `ionic_rdma: disagrees about version of symbol
  ionic_adminq_post_wait / ionic_intr_free / ionic_put_cmb` (`B/evidence/B-driver/load.txt`).
  The rebuilt one is then inserted by hand. It is harmless only because devmem-tx changes the
  CRCs; a rebuilt ionic with unchanged CRCs would get the *baked* `ionic_rdma` first and the
  `insmod` of the rebuilt one would fail with "File exists".
- **F3. Rx loss under sustained TSO traffic shows up differently on a P4-enabled server.**
  In the 360 s run the receiver's emulator dropped **399 of 55.15 million frames**; sender
  `TcpRetransSegs` +399, receiver `hw_rx_dropped` +399, netdev `rx_dropped` +399, emulator
  `frames_rx_dropped` +399 and `rx_ucast_drop_packets` 399, but **`rx_queue_empty` stayed 0**
  (`B/evidence/devmem-tx/M10-sustained.txt`, `ernic-2-stats-after-matrix.txt`). Every byte was
  still verified. This is the overrun of `notes/test-emulator-offloads.md` F2 with a different
  drop site: this stack's servers run with `--p4` (upstream's default fleet), the P4 Rx filter
  claims every TAP frame (`R/src/ionic/ionic_eth_p4.c`, `eth_p4_frame()`), and re-queues it
  through `ionic_eth_emu_queue_rx_frame()` into the 256-frame inbox
  (`ETH_INBOX_MAX_FRAMES`, `R/src/ionic/ionic_eth_emu_priv.h:550`). When the guest has posted no
  buffer the frame is put back at the head of the inbox ("not a drop",
  `ionic_eth_emu.c` poll loop), so the ring-empty counter never moves; the inbox fills and
  `ionic_emu_stats_rx_inbox_full()` drops and counts at the port and the class counters only.
  So with P4 on, the guest's 1024-entry ring is backed by 256 more frames and the loss is
  reported as `frames_rx_dropped` / `rx_ucast_drop_packets`, never `rx_queue_empty`. The earlier
  note's proposed documentation line should name those counters too. Reproducer:
  `B/scripts/devmem-tx/m10.sh 360` (plain `iperf3` should do the same; NOT RUN). Not seen in any
  upstream test or in the shorter devmem rows. Ring-size dependence on this stack: NOT RUN.
- **F4. qemu-minimal's bake stages the p4 patch as `0010`, and srq now has a `0010` and a
  `0011`.** `Q/ansible/playbooks/vm-rocm-golden.yml:413-439` copies `srq/*.patch` and renames
  the p4 patch by adding 9 ("0001-0009" in its comment). With `feat/devmem-poc`'s srq the p4
  patch becomes `0010-ionic-register-a-P4-auxiliary-device.patch` and sorts **between** srq 0009
  and srq `0010-ionic-use-...` (`r` < `u`). Checked here: p4 before srq 0010/0011 still applies
  and gives the same tree as srq 0001-0011 then p4, so nothing breaks today, but the order is no
  longer the intended one and a devmem series would need numbers after both. Moving
  `ernic_source_pin` past 2d3a3d02 should come with a counter-based renumbering.
- **F5. `srcversion` cannot tell an `ionic_rdma` or `ernic_p4` built against a different ionic
  from the baked one.** Both keep `24E57766...` / `66D2E219...` after being recompiled against
  the devmem tree (their own `.c` files did not change), while the CRCs they import did.
  `/etc/ionic-srq-p4.json` and `Q/qemu/checks/rocm-golden.sh` identify the modules by
  srcversion; for these two only the build-id (or the import CRCs, `modprobe --dump-modversions`)
  shows which ionic they were built for.
- **F6. The baked `ernic_p4.ko` must be rebuilt with the series** (section 3.1): confirmed by
  the refused load, as the series' README says.
- **F7. Harness facts for this stack.** No `/mnt/hostfs` in the guests (scp instead); the
  management NIC is `enp0s6`; `bench.sh`'s P4 step leaves the fp8 program in NIC 1 (cycle
  afterwards); a `quick-start test` does not reload or check the ionic modules, so it runs
  against whatever is loaded.
- Carried over, unchanged by this work: baseline F1 to F4 (docker shim, the 192.168.100.x GID,
  buildx, `quick-start up` on a running stack).

## 6. Could `quick-start test` grow `--devmem-test`? Yes. Proposal (not implemented)

How the existing options work, from `Q/qemu/qemu-tool/cli.py:478-500`,
`Q/qemu/qemu-tool/quick_start.py` and `Q/ansible/playbooks/vm-quickstart.yml`:

1. `cli.py` adds a `store_true` flag and passes it to the quick-start state
   (`p4_test=args.p4_test`, `:1590`).
2. `_check_optional_tests()` (`quick_start.py:1338`) refuses the flag before anything runs when
   the merged fleet settings cannot support it (`--p4-test` needs `<p4-enable>` and
   `<p4-guest-scope>load`).
3. The playbook command gets one extra var (`-e quickstart_p4_test=true`, `:1479`).
4. The playbook has one play per optional test, gated by that var with `meta: end_play`
   (`vm-quickstart.yml:681`), whose first tasks check the guest can run it and fail with the
   reason; the test's one-line result is written to an artifact file
   (`<vm>.md` for a per-guest test; `rocm-rdma.md` for the two-guest one, `:2090-2124`).
5. `_report_optional_tests()` (`quick_start.py:1602`) greps that line with a regex and **fails
   if a requested test left none**; the facts play also records it as a node-exporter textfile.

Only the ROCm RDMA play has a tag, and it is the model for a two-guest test: it runs on
`fleet[0]`, starts the peer's side as a transient `systemd-run` unit on `fleet[-1]` with
`delegate_to`, waits for the listening socket with `ss` (never by connecting), runs the client
under `timeout`, reads the server's output back, and writes one result line.

### 6.1 quick-start side

- `cli.py`: `--devmem-test` ("Send TCP from a dma-buf on the first guest's NIC to the last guest
  and verify every byte there. Needs two guests, an ernic server that grants Tx offloads and a
  guest image whose ionic carries the devmem-tx series. Off by default.").
- `_State.devmem_test`; add `("--devmem-test", state.devmem_test)` to `asked`.
- `_check_optional_tests()`: refuse when the fleet has fewer than two guests or
  `<ernic-tap>false` (no Ethernet path between the guests; the RDMA mesh does not carry TCP).
  quick-start cannot see from the fleet file whether `<ernic-ref>` grants offloads or the image
  has the series, so those two are the play's first checks, as `/dev/ernic-p4` is for P4.
- `_playbook_command()`: `-e quickstart_devmem_test=true`.
- `_DEVMEM_RESULT = r"^devmem tx: pass, (\d+) bytes verified at (\S+), (\d+) sends, (\d+)
  completions, 0 copied, (\d+) unreadable skbs, ([\d.]+) MiB/s"` in `devmem-tx.md`;
  `_report_optional_tests()` dies without it; add the flag to the "What else is here" hint.

### 6.2 The play (tag `devmem`, after the ROCm RDMA play; `hosts: fleet[0]`, `become: true` for the sender)

1. `meta: end_play` unless `quickstart_devmem_test`.
2. **Can this stack run it?** One shell task per guest, then an `assert` that prints the reason:
   - `/etc/ionic-srq-p4.json` says `"devmem_tx": true` (stamp, 6.3), and
     `ethtool -S <if> | grep -q tx_unreadable_drop` on the sender (the loaded driver has the
     series; that counter exists only with patch 0005);
   - `ethtool -k <if>`: `tx-scatter-gather: on` and `tx-checksum-ip-generic: on`; otherwise
     "the stack's `<ernic-ref>` server grants no Tx offloads: it predates rocm-ernic's
     transmit-offload change" (the exact state of the published 10.2 stack today, where the bind
     returns `EOPNOTSUPP`);
   - `/dev/udmabuf` exists, `grep -q CONFIG_NET_DEVMEM=y /boot/config-$(uname -r)`,
     `command -v devmem_tx`.
   The interface is found by MAC prefix `72:6f:63:6d`, as the configure play does; the peer's
   address is `hostvars[groups['fleet'][-1]].fleet_guest_ip`.
3. Clear a leftover unit; start the receiver on `fleet[-1]`:
   `systemd-run --unit=quickstart-devmem-rx --collect devmem_tx -r -p 5310 -s <peer ip> <plan> -W 20`
   with output to a file; wait with `ss -ltn 'sport = :5310'`.
4. Counter snapshot on both guests (`ethtool -S`: `tx_unreadable_drop`, `hw_rx_dropped`).
5. Optional proof step, on by default (`quickstart_devmem_probe: true`; bpftrace is already in
   the image and kprobes with vmlinux BTF work on this guest): a transient unit running a
   20-line bpftrace script on `kprobe:ionic_start_xmit` that counts unreadable skbs and sums
   their `data_len`, stopped after the send.
6. The send, twice, under `timeout 120`: TSO on, then `ethtool -K <if> tso off` (restored in an
   `always:` block). Plan for both sides: `-S 16m -o 123 -l 100003 -a 7919 -n 600` (57 MiB,
   nothing page-aligned; 0.25 s here).
7. Wait for the receiver unit to end; read both outputs.
8. **Assertions** (each with its own message):
   - sender `RESULT tx PASS`, `completed == sends`, `copied=0`, `compl_errors=0`, `compl_gaps=0`;
   - receiver `RESULT rx PASS`, `bytes == expected`, `mismatches=0` (the receiver regenerates
     the stream from the plan; the verdict is taken from the **receiver's** text);
   - sender `tx_unreadable_drop` unchanged;
   - with the probe: unreadable skbs > 0 and unreadable payload bytes >= bytes verified;
   - no new `BUG|WARNING|Oops|Call Trace` line in either guest's kernel log since a marker.
   A bind refused with `Driver does not support netmem TX` is reported as "known limitation" in
   the result line, the way the `vram` RDMA mode is, so the report says why.
9. Write `devmem-tx.md` to the controller and the textfile metrics
   (`quickstart_devmem_tx_{bytes,bytes_per_second,timestamp_seconds}`).

It is self-contained (creates its own udmabuf, leaves nothing in the NIC, about 10 s), does not
need the GPU, and can run alone with `--tags devmem` on a configured stack. Deliberately not in
it: the 16/17-frag and offload-off rows (they wait on TCP timeouts), anything with `rmmod`
(core use-after-free on v7.2.x), and an amdgpu-sourced buffer (GTT, not VRAM, until the guest
kernel has the v7.4 dynamic attach; a later `--devmem-mode gpu` could add `devmem_tx_gpu -G`).

### 6.3 What must be baked into the guest image (`Q/ansible/playbooks/vm-rocm-golden.yml`) and the stack

- **`ernic_source_pin`** (`:104`) at a rocm-ernic commit that has `patches/devmem-tx` (2d3a3d02
  or later), and **`<ernic-ref>`** in `Q/qemu/stacks/rocm-golden.xml` at a fork server image
  built from a commit with the Tx offloads. Without the second the test can only report the
  known limitation.
- **The ionic series**: add a third loop to "Stage the combined SRQ + P4 ionic patch series"
  (`:413-439`) copying `devmem-tx/*.patch` after the p4 patch, and replace the `+9` renumbering
  with a running counter (F4): srq 0001-0011, p4 0012, devmem-tx 0013-0019.
- **`ernic_p4.ko` against the same tree**: append `{{ ionic_p4_guest_patches_root }}/devmem-tx`
  to `ernic_p4_dkms_patches_dirs` (`:612-614`). Shown necessary here (F6).
- **The stamp and the image check**: `"devmem_tx": true` and the new `kernel_patches` text in
  `/etc/ionic-srq-p4.json`; in `Q/qemu/checks/rocm-golden.sh`,
  `strings ionic.ko | grep -q tx_unreadable_drop` (present 16 times in the devmem module, 0 in
  the baked one; the series adds no function symbol `nm` could find) and, given F5, a check that
  `ernic_p4.ko` imports the CRC the installed ionic exports.
- **The test program**: `devmem_tx` (one C file, no library beyond libc: udmabuf, raw
  generic-netlink `NETDEV_CMD_BIND_TX`, `MSG_ZEROCOPY` + `SCM_DEVMEM_DMABUF`, self-verifying
  receiver; source `/var/tmp/ionic-devmem/test/src/devmem_tx.c`). The natural home is
  rocm-ernic's `tests/`, built in the bake the way `tests/p4_fp8_client.c` is (it is in the
  source archive at `:562-577`) and installed as `/usr/local/bin/devmem_tx`. It would first
  have to meet rocm-ernic's rules for guest test programs (C11, no `_GNU_SOURCE`,
  `parse_int.h`, `ernic_log`). The alternative, the kernel selftest `ncdevmem`, needs the YNL
  build from a full kernel tree and has no verifying receiver; it was used here only as a
  second opinion on the bind.
- **Already there**: `CONFIG_NET_DEVMEM=y`, `CONFIG_UDMABUF=y`, `/dev/udmabuf`, bpftrace, kprobes
  and vmlinux BTF. **Missing, and worth adding to a debug flavour only**: `pahole` (module BTF)
  and a kernel with `CONFIG_DMA_API_DEBUG`.
- A boot unit or udev rule is **not** needed: the test runs as root through `become`.

### 6.4 What this run says about feasibility

Everything the play would do was done by hand on the published image with the two swaps:
the bind, both sends, the receiver verdict, the counter and probe checks, and all of
quick-start's existing tests in the same boot. The one thing a lane would have to watch is F3:
a long or repeated send can show `hw_rx_dropped` on a busy runner, so the play should assert on
the receiver's verdict and the sender's completions, not on zero retransmits.

## 7. Scripts added (`B/scripts/`), originals untouched

    build-ernic.sh LABEL REF          server from `git archive REF`, built in the stack's ernic image (BUILD_TYPE=Release|Debug)
    stack.sh server LABEL|image       choose the server binary for the next `up` (new subcommand)
    ionic-mod.sh stage|build|load|verify|restore   patched ionic + ionic_rdma + ernic_p4, in-guest build, insmod
    qs-all.sh PREFIX                  the baseline's quick-start sequence + SRQ + hashed TCP
    qs-results.sh LABEL...            one line of numbers per quick-start run
    srq-test.sh LABEL                 max_srq, ib_send_bw --use-srq, ibv_srq_pingpong (GID index looked up)
    bench.sh LABEL [N]                repeated ib_send_bw / SRQ / P4 samples with host load
    devmem-tx/{install,smoke,m3-m5,m6,m7,m9,m10,run-matrix}.sh, lib.sh, probe.bt, summarize.py, pcapsum.py

Evidence: `B/evidence/{A-emulator,B-driver,quickstart/{A,A2,B,B2,final}-*,srq,bench,devmem-tx,final,facts/30-final-devmem-*}`;
builds `B/ernic/{poc,poc-rel,base-rel}`; staged trees `B/ionic-mod/{baked-check,srqfix,devmem}`
(`srqfix` = srq 0001-0011 + p4 without devmem-tx: staged, NOT built or run).
