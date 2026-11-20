# Stack 3 (`ionic-devmem-r102`): independent verification of the baseline, devmem and GPU reports

2026-11-20, 02:16Z to 03:18Z (evidence stamped in UTC). `B` = `/var/tmp/ionic-devmem/test-rocm102`,
`V` = `B/evidence/verify`, scripts `B/scripts/verify/`. Reports checked:
[rocm102-baseline.md](rocm102-baseline.md), [rocm102-devmem.md](rocm102-devmem.md),
[rocm102-gpu.md](rocm102-gpu.md). Nothing in rocm-ernic, qemu-minimal or stacks 1 and 2 was
changed; no product code was fixed. Everything below was RUN by me unless marked
**evidence only** (I read their file, did not repeat the test) or **NOT RUN**.

**Result.** No claimed PASS was refuted. Every pass I re-ran with my own tools passed again, and
the builds are what the reports say they are. Four claims are overstated or rest on weak
evidence (section 2), one inconsistency is in the GPU note, and one behaviour they class as a
"clean counted drop" kills the connection in a case a real sender can hit (F1).

## 1. Identity: is the stack what they say it is?

| Item | Check | Verdict |
| --- | --- | --- |
| Running NIC servers | `/proc/1/exe` in both `ernic-N` containers = sha256 `d50b1d29...` = `B/ernic/poc-rel/build/rocm-ernic`. `B/ernic/poc-rel/src` is `diff -r`-identical to my own `git archive 2d3a3d02`. **I rebuilt it** (`build-ernic.sh verify-rel 2d3a3d02`, Release, same image): **bit-identical** (`cmp`). Arguments unchanged, `nic_profile : compat` | CONFIRMED (`V/01-*.txt`, `V/02-*.txt`) |
| Loaded ionic | Both guests: srcversion `08D37B49...`, build-id `412941e1...` = `~/ionic-mod/devmem/mod/ionic.ko`; `ionic_rdma` `8cfe655d...`, `ernic_p4` `757781e4...` likewise. The 43 source files in the guests' `~/ionic-mod/devmem/mod` hash equal to **my own staging** of v7.2.6 + srq 0001-0011 + p4 + devmem-tx 0001-0007 from my archive | CONFIRMED (`V/00-identity.txt`, `V/03-ionic-source-check.txt`) |
| Guest image | `/etc/rocm-golden-guest.json`: ROCm 10.2.0-2, kernel `7.2.6-070206-generic`, amdgpu-dkms `1:7.2.3.31700000`, four emu patches, `ernic_source_pin 90434a2`; `/etc/ionic-srq-p4.json` srcversions = the on-disk DKMS modules; overlays back onto `r102-1-backing.qcow2`, whose `.image-ref` is `...qemu-tool-rocm-golden:20261119.g2aea67d`, pulled with digest `sha256:14b3785b...` (`B/logs/up-1.log`) | CONFIRMED. Not pristine any more, as reported: r102-1 carries `linux-image-7.2.6-devmemp2p` (not booted) and tool directories |
| qemu-tool | checkout at `v2.1.0`, clean; other images as published | CONFIRMED |
| Image server is 47 commits behind, bd902345 is 4 | `git rev-list --count` | CONFIRMED |
| Final base test really ran on the devmem ionic | guest dmesg: modules loaded 02:10:28Z, `gpu-final-base` started 02:10:31Z | CONFIRMED |

## 2. Claims

### Baseline report

| Claim | Verdict | Basis |
| --- | --- | --- |
| Guest and server facts (kernel config, DKMS, `max_srq` 256, no socat/pahole, no module BTF, BAR0 256G at `0x4000000000`, `rocjitsu-vram` mapped in the server) | CONFIRMED | re-read in the guests and containers |
| Upstream reference table (base, sdxi, hipfile, RDMA x3, p4, after-cycle) and the 1134-1950 band | CONFIRMED | every number is in `B/evidence/quickstart/<label>/*.md` with the matching option in `run.txt`; evidence only for the image-server + baked-ionic runs |
| Bind fails with `EOPNOTSUPP` on the unmodified guest; controls pass | CONFIRMED, evidence only | `devmem-baseline/baseline-stock-ionic.txt` |
| Non-default `<ernic-subnet>` breaks the RDMA step; `quick-start up` on a running stack destroys it | evidence only, NOT RE-RUN | logs show what is claimed |

### Devmem report

| Claim | Verdict | Basis |
| --- | --- | --- |
| Offloads appear with the unmodified ionic on the new server | CONFIRMED | baked ionic on the 2d3a3d02 server (my header-split boot): Tx offloads on |
| Regression table verdicts (every PASS / expected FAIL) | CONFIRMED | files audited; like for like (same options, same guests, same order, `nic_profile compat` in every run; server hash and ionic srcversion recorded) |
| Upstream tests pass on the devmem configuration | CONFIRMED, re-run | 12 `quick-start test` runs: base x2, dmabuf x4, host-alloc, `--p4-test`, hipfile x3, sdxi: all rc 0 (`V/20`, `V/22`, `V/23`) |
| SRQ passes | CONFIRMED, re-run | `srq-test.sh` on devmem config, twice (`V/21`), and once on the header-split server |
| P4 passes with the rebuilt `ernic_p4.ko`; baked one refuses | CONFIRMED | `--p4-test` 171.0 MB/s; refusal seen in every load's dmesg |
| devmem TX verified with TSO on and off | CONFIRMED, re-run with **my own tools** (`vtx` random data + expected file, `vrx.py` sha256, `vprobe.bt`) | 5 cases, 12 to 128 MiB, receiver sha256 = expected, 0 zero runs; every data skb unreadable, every frag `net_iov` (`V/10-devmem-tso.txt`) |
| tx off / sg off / both: no zero-filled payload accepted | CONFIRMED, and **shown non-vacuous** | With the series: receiver got 0 bytes (before connect) or a verified prefix (mid-stream), 0 zero runs, in all 6 cases (`V/11`). **Control I built** (same series without 0006, r102-1 only): `tx off sg off` delivers **88002640 bytes "complete" with 2388 zero runs and a wrong sha256**, and 1998705 corrupt bytes before connect (`V/14`). The trap is real on this image and 0006 closes it |
| 16/17 frags: "a clean counted drop ... next positive run passes" | CONFIRMED, but **understated** | see F1 |
| "the exact 15-frag non-GSO limit" passes | WEAK EVIDENCE | true only with one send at a time (`-w 0`); see F1 |
| 360 s run, 74.1 GiB, 399 drops with `rx_queue_empty` 0 | CONFIRMED, evidence only | `M10-sustained.txt`; source lines as cited |
| "35 cases" | CONFIRMED | `SUMMARY.txt` has 35 lines; 13 of them are the negative and offload-off rows |
| "No rate moved beyond noise" / server "not shown, not excluded" | WEAK EVIDENCE as written, **now measured** | see section 3 |
| hipFile fio 210 -> 264 is "run-to-run" | WEAK EVIDENCE as written (asserted, baseline never repeated); **now measured and true** | three identical runs gave 205 to 433 MB/s, section 3 |
| srcversion blind to rebuilt `ionic_rdma` / `ernic_p4`; bake renumbering (`+9`) | CONFIRMED | loaded vs on-disk srcversions equal with different build-ids; `vm-rocm-golden.yml:413-439` |

### GPU report

| Claim | Verdict | Basis |
| --- | --- | --- |
| GEM buffer on stock 7.2.6: verified payload, placement GTT / system RAM | CONFIRMED, re-run with **my own probe** (`vgpu.bt`) | 1339479436 bytes verified; `dma_buf_dynamic_attach` with `importer_ops == NULL`; `amdgpu_bo_pin` domain 0x2; `GTT pin count 1` while bound; **352447 of 352447** frag addresses in 6-7 GiB = System RAM (`/proc/iomem`), none at `0x40...` (`V/40`). `-J` control the same (`V/41`) |
| `hipMalloc` buffer: verified payload, system RAM, HIP's next read fails | CONFIRMED, re-run once | 1717151513 bytes verified, GTT, 451783 System RAM frags, `evicted_ms` 106, device-to-host readback untouched in 3 reads, recovers after a rewrite (`V/42`) |
| Deciding line `net/core/devmem.c:229`, `dma-buf.c:1048-1049`, `:1081-1085`, `:1201-1202` in v7.2.6; guest `amdgpu_dma_buf.c:357-358` | CONFIRMED | read in `linux-7.2.6` (tag v7.2.6, clean) and in the guest's DKMS source (`V/30-source-check.txt`) |
| One commit `69ab87acc614`, first in v7.4-rc1, +37 -10 | CONFIRMED | `git show --stat`, `git tag --contains`, `git log -S` for three symbols |
| RDMA path is VRAM: 33 of 33 MR entries in BAR0 | CONFIRMED for R3 and P8, both guests | entries `0x4038000000`..`0x403fa00000`, inside BAR0 and inside the 1024 MiB amdgpu registers for P2P. **R1 shows no page-table data and R2 only the header line**: the "33 of 33" rests on R3/P8, not on the two quick-start runs the note cites first. I also ran dmabuf RDMA with validation during a devmem stream (X1) |
| Patched-kernel results (P0-P9) and the host-side memfd hash | CONFIRMED, evidence only | files show what is claimed; NOT RE-RUN (needs r102-1 on the other kernel) |
| "srq 0001-0008 are already upstream in v7.4-rc3" | CONFIRMED by subject, WEAK for content | all eight subjects are in `v7.2..v7.4-rc3`; but their own apply check shows only 0003, 0004, 0008 reverse cleanly, so upstream's versions of the other five differ from the carried ones. Nothing was built |
| HIP failure counts | INCONSISTENT inside the note | the answer table says "copied nothing (3 runs) or hung (1 run)"; section 3 shows 2 "copies nothing", 1 "differs", 1 hung, 1 fine (GPU-kernel fill) |
| Estimates (2-4 days, 1-2 weeks) | not verifiable | opinion |

## 3. Variance, measured

**Server (image 90434a2 vs 2d3a3d02), baked ionic, alternated over six fresh boots**
(`alt-bench.sh`, their `bench.sh` per boot; `B/evidence/bench/verify-alt-*`, `V/71`). Medians:

| Boot | Server | `ib_send_bw` x1000 | x30000 | SRQ | P4 MB/s |
| --- | --- | --- | --- | --- | --- |
| 1 | image | 1323 | 1822 | 1381 | 170.6 |
| 2 | 2d3a3d02 | 1506 | 1759 | 1540 | 172.8 |
| 3 | image | 1409 | 1595 | 1340 | 169.6 |
| 4 | 2d3a3d02 | 1154 | 1734 | 1669 | 173.7 |
| 5 | image | 1489 | 1604 | 913 | 169.2 |
| 6 | 2d3a3d02 | 896 | 1698 | 1822 | 172.8 |

- **No server effect is detectable.** On the least noisy measure (30000 iterations) the image
  gives 1595-1822 and 2d3a3d02 1698-1759; the ranges overlap and the new server is not the slower.
- **Their one image block (1967) was noise**: three more image boots gave 1323-1489 on the same
  measure. The "image server was the highest" caveat can be dropped.
- **P4 is 2 % higher on 2d3a3d02 in every pair** (169-171 vs 173-174). Not a regression.
- This is still a loaded host (load average 7 to 16), not the idle host a rate claim needs.

**Single quick-start samples, devmem configuration** (`V/20`, `V/23`):

| Number | Reported (baseline / A / B) | My repeats |
| --- | --- | --- |
| ROCm RDMA dmabuf MiB/s | 1305 / 1816 / 1612 | 1358, 1225, 1183, 1293 (and 1607 on the header-split boot) |
| ROCm RDMA host-alloc | 1158 / 1987 / 1535 | 1656 |
| `ib_send_bw` in the runs | 1134-1950 / 687-2156 / 731-1427 | 921-1841 over 9 runs |
| P4 | 170.5 / 149.6 / 173.3 | 171.0 |
| hipFile fio MB/s | 212, 214 / 210, 210 / 263, 264 and 236, 237 | **205, 205; 248, 244; 422, 433** in three identical runs on one boot |

The reported RDMA and hipFile deltas are inside the spread of identical runs (hipFile fio spans 2x
with nothing changed). They show nothing either way.

## 4. What they did not try

| Test | Result | Evidence |
| --- | --- | --- |
| devmem TX (16 s, 2.25 GB) **while ROCm dma-buf `ib_write_bw` with validation runs on the same NIC** | both pass: receiver sha256 equal, `VALIDATION: PASSED`, 1651 MiB/s, no emulator error | `V/50` X1 |
| devmem TX (2.0 GB) while `ib_send_bw --use-srq -D 10` runs | both pass | `V/50` X2 |
| devmem TX with `ernic_p4` loaded and the fp8 program in the sender's NIC | pass | `V/51` P1 |
| devmem TX (1.84 GB) while `p4_fp8_bench` runs | both pass; the bench drops to 112 MB/s while sharing the vCPUs | `V/51` P2 |
| devmem TX 2 -> 1, the P4-program NIC receiving | pass | `V/51` P3 |
| **Header-split profile with the unmodified (baked) driver** | harmless: `ethtool -k` differs from compat in exactly one line, `rx-checksumming: on`; hashed TCP both ways, base test, dmabuf RDMA (1607 MiB/s) and SRQ pass; `rx_csum_complete` 95k, `rx_csum_error` 0, `TcpInCsumErrors` 0, no splat | `V/60-*` |
| Unpaced 15-frag sends | F1 | `V/11`, `V/12` |

NOT RUN: header-split with the devmem ionic; devmem TX during hipFile or SDXI; IPv6; MTU rows;
rmmod rows; anything on the patched kernel; the baseline's image-server tests; a Debug server.

## 5. Findings

- **F1. A legal sender can kill its own connection through the 16-frag drop.** My first
  "exact limit" run sent 15 iovecs of 64 bytes (one per page) with `TCP_NODELAY`, back to back,
  without waiting for completions. TCP appended the next send to the unsent skb, made 17-frag
  non-GSO skbs, the driver dropped each (`tx_unreadable_drop` +7), every retransmit was the same
  skb, and `sendmsg` ended with `ETIMEDOUT` after 13 s; the receiver got nothing (`V/11`,
  case `F-15-frags-non-gso-limit`). The same plan with 3 ms between sends passes, 300 skbs of
  exactly 15 frags (`V/12`). So their `M6g` pass depends on `-w 0` (one send in flight), and
  "clean counted drop" means "this connection is dead", not "this skb is lost". The limitation
  is in patch 0005/0007's commit text; the README's test summary does not say a connection
  cannot recover. Not a new bug, but a `--devmem-test` or a user sending small scattered pieces
  will meet it.
- **F2. The offload-off trap is live on this image** (control in section 2). Any bake that
  ships 0007 without 0006 sends zeroes with a valid checksum.
- **F3. Stacks 1 and 2 were restarted during my run by another session**, at 02:17:21Z and
  02:28:57Z (`V/92`); I ran nothing against them. Host load reached 15.9 then.
- **F4. Evidence gap in the GPU note:** R1/R2 lack the MR page-table rows (probe fixed later).

## 6. State left

- **Stack:** running, cold-cycled 03:05Z, override file identical to how I found it
  (`V/docker-compose.override.yml.as-found`).
- **Servers:** 2d3a3d02 Release (`d50b1d29...`), `nic_profile compat`, no P4 program in NIC 1.
- **Guests:** stock `7.2.6-070206-generic`; devmem ionic, `ionic_rdma`, `ernic_p4` loaded by
  `insmod`, build-ids verified; amdgpu loaded; 10.102.100.11 / .12; offloads at defaults;
  `tcp_retries2` 15; IPv4 GID at index 1 on both; 0 dma-buf objects; no kernel splat.
- **Last checks in this state:** base `quick-start test`, devmem TSO on and off (sha256),
  SRQ, hashed TCP both ways: all pass (`V/90`, `V/91`); then hipfile x3 and sdxi (`V/23`), and a last state check (`V/93`).
- **Added by me:** guests: `/usr/local/bin/{vtx,vrx.py,vprobe.bt}`, r102-1 also `vgpu.bt` and
  `~/ionic-mod/vfy-nobackstop`. Host: `B/ernic/verify-rel`, `B/ionic-mod/vfy-{devmem,nobackstop}`,
  `B/scripts/verify/`, `B/evidence/{verify,bench/verify-alt-*,quickstart/verify-*,srq/verify-*,gpu/verify-hip-stock*}`.

## 7. Scripts (`B/scripts/verify/`)

    v.sh                 vcase harness (adapted from test/evidence/tx-verify/src/v.sh): vtx, vrx.py, vprobe.bt
    c-offloads-off.sh    offload-off trap + frag overflow; TAG/COMBOS/NOFRAG for the control
    gpu-case.sh, vgpu.bt GEM-buffer send with my own placement probe
    x-concurrent.sh      devmem TX during ROCm dma-buf RDMA and during SRQ send
    p4-active.sh         devmem TX with the fp8 program loaded, both directions
    vserver.sh           poc-rel | image | header-split (hs-wrapper.sh) for the next up
    alt-bench.sh         server alternation over fresh boots
