# Stack 3 (`ionic-devmem-r102`): the frag-limit fix, proved on the ROCm 10.2 guest

2026-11-20, 06:50Z to 07:51Z. `B` = `/var/tmp/ionic-devmem/test-rocm102`, `F` =
`/var/tmp/ionic-devmem/overnight2/frag`, `E` = `F/evidence/run`. Reads on from
[impl-frag-limit.md](impl-frag-limit.md) (the patch, compile-tested) and
[rocm102-rx.md](rocm102-rx.md) (the state the stack was in). Everything below was RUN by me
unless marked **NOT RUN**. Nothing was committed or posted; rocm-ernic, qemu-minimal and
stacks 1 and 2 were not touched.

## 1. Result

**The fix works as written. No change was made to its code.** Every send that stalled
without it completes with it, every byte verified at the receiver, in one boot, on the same
kernel, with the RX series loaded as well.

- The driver change was not repaired, because nothing was found wrong with it.
- **One thing was added:** a fourth patch form. The RX set was loaded and
  `F/patches/v7.2.6/0008` does not apply on it (`ionic_lif.h` and `ionic_stats.c` context, as
  its author predicted), so I generated
  `F/patches/v7.2.6-on-rx/0009-ionic-send-an-unreadable-skb-with-too-many-frags-as-a-tso.patch`
  with the author's `edit0007.py` on the staged `rxbp` tree (`F/scripts/mk-onrx-726.sh`). Its
  `+`/`-` lines are identical to the v7.2.6 form's. checkpatch `--strict`: only the absent
  Signed-off-by; a W=1 build in r102-1 has 0 warnings (`E/63`).
- **The TX-only form (`v7.2.6/0008` on the `devmem` set) was NOT built or run in a guest.**
  What ran is RX + fix.

## 2. What is loaded now

| Item | State |
| --- | --- |
| Module set | `rxfrag` = v7.2.6 + srq 0001-0011 + p4 + devmem-tx 0001-0007 + RX backport 0001-0008 + frag fix, from `~/ionic-mod/rxfrag` by `insmod` (not persistent) |
| ionic | srcversion `0FDEA047AB0132E8F7AB9C8`, build-id `626c0279...`; `ionic_rdma` `631bb321...`, `ernic_p4` `3e60a75f...`; loaded = built, 6 of 6 (`E/90`) |
| Previous sets | `rxbp` (RX without the fix), `devmem`, `rxbp-p1` still in `~/ionic-mod` and `B/ionic-mod`; `B/scripts/ionic-mod.sh load rxbp` goes back |
| Guests | both `7.2.6-devmemp2p`, MTU 1500, 4 channels, rings 1024/1024, `tcp-data-split off`, offloads at defaults, `tcp_retries2` 15, amdgpu loaded, IPv4 GID present, port ACTIVE |
| Servers | unchanged: 2d3a3d02 Release through `hs-wrapper.sh`, `compat + header-split`, no P4 program |
| L2 | back at MTU 1500 (it was raised to 9000 for the jumbo cases, section 7) |
| Logs and leaks | 0 splat lines, 0 dma-buf objects, no netem qdisc, no capture process |
| Emulator | `tx_desc_data_error`, `tx_desc_fetch_error`, `tx_dma_error`, `tx_queue_error`, `frames_tx_bad` all 0 on both servers |
| Last checks | base `quick-start test` rc 0 (`ib_send_bw` 1269 MiB/s, one sample), SRQ rc 0, hashed TCP, 60 s over-limit stream (`E/61`, `E/62`, `E/47`) |

Sender counters at the end: `tx_unreadable_reseg` 1081346, `tx_unreadable_drop` 121, every
one of the 121 from a case that is meant to drop (section 5 and the matrix's offload-off rows).

## 3. Before and after

Same boot; "before" = `rxbp` (ionic `7C12352B...`), "after" = `rxfrag`. Sender r102-1,
receiver r102-2, `vtx` (random payload, expected file) and `vrx.py` (sha256). Before:
`tcp_retries2` 5 so the timeout comes in 13 s. "512 pieces" = one `sendmsg` of 512 iovecs.

| Case | Before: userspace | Before: `tx_unreadable_drop` | After: userspace | After: drop / reseg |
| --- | --- | --- | --- | --- |
| 15 pieces of 64 B, unpaced, `TCP_NODELAY`, MTU 1500 | `ETIMEDOUT` after 12.8 s, 2880 B accepted, receiver 960 B | +7 | 288000 of 288000 B, sha256 equal, 300 of 300 completions | +0 / +51 |
| 512 pieces of 64 B, MTU 1500 | `ETIMEDOUT`, receiver 0 B | +8 | 2097152 B, sha256 equal | +0 / +1921 |
| 512 pieces of 100 B, MTU 1500 | `ETIMEDOUT`, receiver 3400 B | +7 | 3276800 B, sha256 equal | +0 / +705 |
| 512 pieces of 128 B, MTU 1500 | completes (4 MiB, sha256 equal) | +0 | completes | +0 / +0 |
| 15 pieces of 64 B, unpaced, MTU 9000 | `ETIMEDOUT`, receiver 960 B | +11 | 288000 B, sha256 equal | +0 / +220 |
| 512 pieces of 128 B, MTU 9000 | `ETIMEDOUT`, receiver 0 B | +11 | 4194304 B, sha256 equal | +0 / +1984 |
| 512 pieces of 512 B, MTU 9000 | `ETIMEDOUT`, receiver 0 B | +11 | 16777216 B, sha256 equal | +0 / +2142 |

- Before: `TCPAbortOnTimeout` +1 and `TcpRetransSegs` +6 or +7 per stalled case; what the
  receiver got was a verified prefix; emulator errors 0 (`E/10`, `E/12`).
- **128 B at MTU 1500 never stalled**: a 1448-byte segment holds 11 or 12 such pieces. It is a
  control, not a reproduction.
- After: `copied=0`, emulator error counters +0, 0 retransmissions, 0 splat lines in every
  case (`E/21`, `E/22`).

More layouts with the fix (`E/23`, `E/24`, `E/47`), all verified complete, drop +0:

| Layout | reseg | Wire |
| --- | --- | --- |
| 16 frags non-GSO | +226 | 960 + 64 |
| 17 x 16 B | +3840 | 240 + 32 |
| 1-byte sends back to back | +6 | segments of 15 B and less |
| pieces of 80 B | +1921 | 1200 + 160 |
| **1400 B + 16 x 1 B in one skb** | +200 | **94 segments of 15 B and one of 6 B per skb** (160 skbs of 17 frags) |
| GSO skb with an over-full segment | +4 of 325 TSOs | 1440 in place of 1448 |
| large sends (control) | +0 | 1448 |
| 60 s, every skb over the limit | +205957 | 231542720 B verified, 13730 of 13730 completions |

## 4. On the wire

Captured on `tap0` of the receiver's emulator (before the guest's GRO), checksums recomputed
by `F/scripts/wire.py`; pcaps in `F/evidence/pcap`.

- **Checksums:** 0 bad IP and 0 bad TCP checksums in every flow, IPv4 and IPv6.
- **Sizes:** a 17 x 64 B skb leaves as 960 + 128; no segment exceeds the chosen mss.
- **PSH and FIN:** only on the last segment of a chain (`960 .` then `128 P.`).
- **Sequence:** contiguous, no gap, no retransmission in the clean runs.
- **DF:** set on every packet.
- **IP IDs repeat, as the commit message says they can.** A chain uses n, n+1, ... and the
  next skb starts at n+1. 51 of 304 IDs were used twice in the first case; 331 of 336 in the
  95-segment case.
- **The emulator refused nothing**, including mss 15 and 95-descriptor chains.

## 5. What is still dropped

Each ended in `ETIMEDOUT` with `tx_unreadable_reseg` +0, and a positive run passed after
(`E/25`, `E/45`, `E/46`).

| Case | `tx_unreadable_drop` |
| --- | --- |
| `ethtool -K tso off`, 16-frag skbs | +8 |
| `tso off`, pieces of 64 B | +8 |
| TX ring 64, the 95-descriptor layout | +7 |
| TX ring 64, pieces of 64 B (2 descriptors) | +0, completes: the ring limit is per layout |
| IPv6 with `tx-tcp6-segmentation off` | +8 |
| IPv4 with DF clear (`ip_no_pmtu_disc=1`) | +8 |

NOT exercised: TSO_ECN off with CWR set (the ECN connection I made never sent CWR on a data
segment, so it completed), tunnels, IPv6 extension headers, VLAN, SYN/RST/URG, the hwtstamp
queue.

## 6. Regression and stress

| Run | Result | Evidence |
| --- | --- | --- |
| devmem TX matrix, 38 rows | 29 complete, 0 mismatches; 9 offload-off rows as before | `F/evidence/matrix/SUMMARY.txt` |
| Matrix `M6n*` (the four old negatives) and the four edge rows | **now complete**, 8 of 8; their expectations need updating | same |
| `tx off` / `sg off` / both | receiver 0 B or a verified prefix; all-zero payloads 0 in 18 captures; bad checksum 0 on `tap0` in 9 | `M7-*.txt` |
| Sustained 300 s | 64819194457 B verified, 64819 of 64819 completions, 0 retransmissions | `M10-sustained.txt` |
| Base `quick-start test`, SRQ, hashed TCP | rc 0, rc 0, pass | `E/61`, `E/62` |
| devmem TX to devmem RX, 4 plans incl. 64 B and 100 B pieces | receiver dma-buf sha256 = copy-mode control, 4 of 4; 0 linear cmsgs | `E/50` |
| IPv6 (TSO6), 5 layouts incl. the 95-segment one | verified complete | `E/45` |
| Tiny pieces and 1 MiB sends on one socket | 1169203200 B verified, reseg +81666 | `E/40` |
| 8 concurrent senders, own dma-bufs | 8 of 8 verified, reseg +190306, 8 binds freed | `E/41` |
| Receiver ring 64 under 8 iperf3 streams | 8453 frames lost; **31 over-limit skbs retransmitted**; verified | `E/42` |
| 3 % netem loss | **2178 over-limit skbs retransmitted, 1008 non-GSO ones trimmed at `snd_una` first** (partial ACK of a re-chunked skb); verified | `E/43` |
| 7 MTU changes mid-stream (9000/1500/3000/4000) | 1268985600 B verified, 196 over-limit retransmissions | `E/44` |

The matrix's three receiver-guest captures with "bad" checksums are GRO-merged frames, the
artefact earlier notes describe; `tap0` has none.

## 7. Findings

- **The author's P6 does not make the 95-descriptor skb.** With `TCP_NODELAY` and a pause
  the 1400-byte send leaves alone. Nagle on and no pause makes it (`frag-run.sh p6b`).
- **`ethtool -G rx 64` alone loses nothing here**: the P4-enabled emulator backs the ring
  with a 256-frame inbox. I added iperf3 load, and netem for loss inside a chain.
- **Cost of tiny pieces:** 3.5 to 7 MiB/s in these runs, single samples; no rate claim.
- **Jumbo frames work on stack 3** once the tap, the in-container bridge, the veths and the
  docker bridge are raised (`F/scripts/jumbo.sh 9000`, run time only, restored with `1500`).
- `tx_tso_bytes` is assigned, not added (`stats->tso_bytes = len`), in the unpatched
  driver too; not this patch's.
- **My harness bugs**, bad runs kept and labelled: a 30 s run filled the guest's 3.9 GB tmpfs
  with the expected file; `retx.bt` probed an inlined function. And one rule slip: a guest
  command began with `rm -rf` of a scratch directory that did not exist yet; it removed
  nothing.

## 8. NOT RUN

The TX-only patch form in a guest; GPU buffers; the lockdep or a KASAN kernel; VLAN;
`--p4-test`, `--sdxi-test`, `--hipfile-test`, ROCm RDMA tests; the RX matrix again on this
set (one TX to RX script only); IPv6 at MTU 9000; real hardware (whether firmware takes an
mss of 15 is still unknown).

## 9. Files

    F/patches/v7.2.6-on-rx/0009-...patch     the form that was built (sha256 554a8731...)
    F/msgs/0007-v7.2.6-on-rx.note            its backport note
    F/scripts/  mk-onrx-726.sh frag-run.sh stress.sh v6.sh tx2rx.sh matrix.sh nodf.sh
                sustained.sh jumbo.sh cap.sh wire.py retx.bt
    F/tools/vtx6.c, vtx6                     the verifier's sender over IPv6
    F/evidence/run/NN-*.txt                  one file per phase; F/evidence/matrix; F/evidence/pcap
    B/ionic-mod/rxfrag                       staged tree (its SOURCE records the mbox before
                                             the note was re-wrapped; the diff is the same)
    guests: ~/ionic-mod/rxfrag, r102-1 also ~/ionic-mod/rxfrag-w1 (W=1 scratch build)
