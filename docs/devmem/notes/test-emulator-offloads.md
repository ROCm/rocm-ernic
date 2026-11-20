# Emulator TX offloads against the stock ionic driver: two-guest validation

Run 2026-11-18, stack 1 (`ionic-devmem`, guests devmem-1/devmem-2). First time the
emulator's SG / CSUM_PARTIAL / TSO v4, v6, ECN / v1 SG change met a real guest.

- Server under test: `/var/tmp/ionic-devmem/test/ernic/tx-offloads-1/build/rocm-ernic`
  (sha256 `29413ea9...7ef118`; source snapshot in `../src`). **Not rebuilt: no emulator
  change was needed.** This is the binary the next tester must use.
- Drivers, all unpatched: ERNIC DKMS-source ionic (v7.2.6 + srq + p4) on
  `7.2.15-devmem`; the image's own DKMS `ionic-ernic/7.2.6` on stock
  `7.2.6-070206-generic`; in-tree ionic on `7.4.0-rc3-devmem-kasan` and (short) on
  `7.2.15-devmem`.
- Evidence root (`E` below): `/var/tmp/ionic-devmem/test/evidence/emu-offloads/`.
  Scripts: `/var/tmp/ionic-devmem/test/scripts/emu-offloads/`.

## Result in one paragraph

No emulator or driver bug found; nothing in rocm-ernic, the mainline series or the
backport was changed. Every hash matched in every transfer (over 200 GiB, both
directions), no capture shows a bad IP/TCP/UDP checksum or an over-MTU frame, and the
emulator's `tx_desc_data_error` / `tx_dma_error` / `tx_desc_fetch_error` stayed 0 on
both servers throughout. Five suite runs failed the harness: two were holes in the
harness's retransmit accounting, one a duplicated FIN, and **two were real loss: the
receiver's Rx ring overran** (`rx_queue_empty` = `hw_rx_dropped` = sender retransmits,
30 and 38 frames in a 1 GiB transfer). That loss is on the receive side and is
congestion, not corruption, but it does not happen with the pre-change server at the
same ring size, so it is a behaviour change of this emulator build that users will
see (section "Findings", F2).

## What one suite does

`run.py LABEL CONFIG:FAMILY...`: one offload configuration x one address family,
both directions. Per direction, with `tcpdump` running on the receiving guest (GRO
off there so wire segments are seen; filter `ether src <sender MAC>`):

- 15 hashed TCP transfers of 1, 2, 100, mss-1, mss, mss+1, 2 mss, 2 mss+1, 44 mss,
  45 mss+1, 65535, 65536, 65537, 1 MiB+1 and 16 MiB bytes;
- many small writes: 395 writes cycling 1/7/100/1448/3000/13 bytes with and without
  `TCP_NODELAY`, 6000 one-byte writes with `TCP_NODELAY`, 100001 writes of 1/2/3 bytes;
- `TCP_MAXSEG` 536 and 88;
- `sendfile()` of 4 MiB from tmpfs and from ext4, and corked `sendfile()` of
  non-adjacent 97-, 64-, 300- and 5000-byte pieces (up to 17 page frags per skb; the
  64-byte case makes the driver linearize);
- `MSG_ZEROCOPY` in 64 KiB, 5000-byte and 1 MB sends, each buffer overwritten as soon
  as its completion notification arrives;
- IPv6 only: TCP behind a destination-options header, hop-by-hop + destination
  options, and corked sendfile behind hop-by-hop;
- UDP stop-and-wait, 45 datagrams from 1 byte to 65507/65527 (both sides of the MTU,
  fragmented ones included), each echoed back as a hash; 4 datagrams whose UDP
  checksum computes to 0;
- ping sweep, 18 sizes around the 60-byte minimum and the MTU, up to 65507/65527,
  plus DF at exactly the MTU;

then, uncaptured, 64 MiB+3 and 1 GiB (256 MiB on VLAN). Payload comes from a seeded
generator; the receiver hashes what arrives, logs it and replies with it, and the
host compares the receiver's log with the sender's list.

A suite PASSes only if: every hash matches; the capture has no bad checksum, no
frame above MTU+14 (+4), no sequence gap, no UDP datagram without checksum, no
TCP payload above the MSS, no IPv4 ID out of order; the emulator's TX/RX error
counters and the guests' `hw_*_dropped`/`rx_csum_error`/`dma_map_err` and
`*InCsumErrors`/`*InHdrErrors`/`TCPTimeouts` do not move; `tx_tso`/`tx_csum` move
or stay flat as the configuration says; and every retransmission by a test socket
(TCP_INFO `total_retrans`, senders and the receivers' reply sockets) is shown to
have been unnecessary: the receiver's capture has the segment twice with no gap, or
(uncaptured phase) the peer DSACKed it.

## Matrix

Columns: suites run / PASS. "1 GiB" = MiB/s of the 1 GiB transfer, range over the
rows (emulation numbers on a shared host, not performance claims).

### A. `7.2.15-devmem`, ERNIC DKMS-source ionic (the fleet's driver source)

`ethtool -k`: tx-checksum-ip-generic, tx-scatter-gather, tx-tcp-segmentation,
tx-tcp-ecn-segmentation, tx-tcp6-segmentation all `on`
(`E/k7215-dkms-ethtool-k.txt`). Two full passes: `E/k7215-dkms/` (first, 20 suites)
and `E/k7215-dkms-final/` (final harness incl. IPv6 extension headers, 21 runs).

| Case | IPv4 | IPv6 | 1 GiB MiB/s | Evidence (`E/k7215-dkms-final/`) |
| --- | --- | --- | --- | --- |
| default (TSO, GSO, SG, csum on), MTU 1500 | PASS x2 | PASS x2 | 164-228 | `default-v{4,6}.json` |
| GSO off (TSO stays on) | PASS x2 | PASS x2, **FAIL x1** (F2), | 170-233 | `gso-off-v{4,6}.json`, FAIL row in `results.tsv` 21:25:13 |
| TSO off | PASS x2 | PASS x2 | 159-232 | `tso-off-*` |
| TSO off + GSO off | PASS x2 | PASS x2 | 164-219 | `tso-gso-off-*` |
| SG off | PASS x2 | PASS x2 | 168-251 | `sg-off-*` (`tx_*_frags` +0) |
| TX checksum off (`tx off`) | PASS x2 | PASS x2 | 171-221 | `tx-off-*` (`tx_csum` +0) |
| ECN negotiated (`tcp_ecn=1`) | PASS x2 | PASS x2 | 180-248 | `ecn-*`; no CWR segment occurred, see D |
| MTU 9000 | PASS x2 | PASS x2 | 309-357 | `mtu9000-*`, max frame 9014 |
| MTU 9198 (device maximum; 9199 refused) | PASS x2 | PASS x2 | 292-360 | `mtu9198-*`, max frame 9212 |
| MTU 9000, TSO off | PASS x2 | PASS x2 | 293-331 | `mtu9000-tso-off-*` |
| iperf3 `-Z` TCP v4/v6/`-P 4`, 10 s | PASS | PASS | 1.36-2.05 Gbit/s | `extras.txt` (0 retransmits; 1 DSACKed probe in one run) |
| iperf3 `-u` 50 Mbit/s, 1400 B and 8000 B (fragments) | PASS | PASS | 0 lost of 22326 / 3908 | `extras.txt`; not payload-verified |
| selftest `msg_zerocopy -z -Z 1` TCP, 2 s | PASS | PASS | `zc=y`, 343-526 MB | `extras.txt`; byte counts only |
| RDMA `rping -C 20 -S 4096` (client validates data) | PASS both directions | | | `extras.txt` |
| kernel log | 0 BUG/WARNING/watchdog lines | | | `E/k7215-dkms/dmesg-check.txt` |

### B. stock `7.2.6-070206-generic`, the image's DKMS ionic (exactly what the fleet boots)

Used for the VLAN and CE-marking rows: the two test kernels have neither
`CONFIG_VLAN_8021Q` nor any AQM qdisc. `E/stock726-dkms/`, `E/stock726-dkms-v6ext/`.

| Case | Result | Evidence |
| --- | --- | --- |
| default, v4 and v6 | PASS, PASS (+ PASS with IPv6 ext headers) | `default-v{4,6}.json` |
| VLAN 100 sub-interface, TSO on, v4 and v6 | PASS, PASS | `default-vlan{4,6}.json`: every frame `ethertype 802.1Q, vlan 100` in-packet, max frame 1518, `tx_tso` +6500..7100, 0 bad checksums |
| VLAN, TSO off, v4 and v6 | PASS, PASS | `tso-off-vlan*` |
| VLAN, MTU 9000, v4 and v6 | PASS, PASS | `mtu9000-vlan*`, max frame 9018 |
| VLAN v6 + IPv6 ext headers | **FAIL x1** (F3, harness), PASS on rerun | `stock726-dkms-v6ext/results.tsv` |
| ECN with CE marks (`fq_codel ce_threshold 1us` at the sender), v4, v6, VLAN v4 | PASS x3 | `ecn-ce-*.json`; `ecn-ce-cwr-check.txt`: 7 TSO bursts whose first segment carries CWR and the rest do not |
| TSO off, v6 + ext headers | PASS | `stock726-dkms-v6ext/tso-off-v6.json` |
| iperf3 `-Z`, iperf3 `-u`, `msg_zerocopy` (`zc=y`), rping | PASS | `extras.txt` |

### C. `7.4.0-rc3-devmem-kasan`, in-tree ionic

`ethtool -k` as in A (`E/k74rc3-intree-ethtool-k.txt`). `E/k74rc3-intree/`.

| Case | IPv4 | IPv6 | 1 GiB MiB/s | Note |
| --- | --- | --- | --- | --- |
| default | PASS | PASS | 176-219 | |
| GSO off | PASS | PASS | 150-224 | |
| TSO off | PASS | PASS | 30-32 | software GSO under KASAN is 6x slower than TSO |
| TSO + GSO off | PASS | PASS | 31-32 | |
| SG off | PASS | PASS | 31-32 | |
| TX checksum off | PASS | PASS | 30-31 | |
| ECN negotiated | **FAIL x1** (F4), PASS on rerun | PASS | 140-212 | `ecn-v4-FAIL-*` |
| MTU 9000 / 9198 / 9000 TSO off | PASS | PASS | 244-260 / 115-125 | |
| iperf3 `-Z` | PASS | **31 retransmits** (F2) | 1.4-2.0 Gbit/s | `extras.txt`, `iperf3-tcp6-rx-overrun-diagnosis.txt` |
| iperf3 `-u`, `msg_zerocopy` (`zc=y`) | PASS | PASS | | `extras.txt` |
| KASAN / lockdep / WARN | none (checked at 48 min uptime, after the matrix) | | | `splat-check.txt` |
| VLAN, ECN with CE marks | NOT RUN | | | no 8021q, no AQM qdisc in this kernel |
| RDMA | NOT RUN | | | no `ionic_rdma` (INFINIBAND_IONIC unset) |

### D. `7.2.15-devmem`, in-tree ionic (the state the stack is left in)

`E/k7215-intree-final/`: default v4 PASS, default v6 **FAIL x1** (F2: 30 frames
dropped at the receiver's Rx ring in the 1 GiB transfer), MTU 9000 v4 PASS, TSO off
v6 PASS. `E/51-final-tcp-test.txt`: `tcp-test.sh 64` PASS both directions.

### Not run / not covered

- Host-side bridge capture: all captures are on the receiving guest (it has no RX
  checksum offload, so `tcpdump -vv` verifies the wire checksum there).
- ECN on the two test kernels negotiates ECN-capable transport but, with no CE marking possible, no
  CWR segment was produced; the TSO-ECN shape is covered only on stock 7.2.6.
- TSO6 shapes the device refuses (routing header with segments left, fragment, AH):
  not generated. The documented `tx_desc_data_error` behaviour is unobserved.
- Hostile/garbage descriptors: unit-test territory, not tried from a guest.
- devmem TX itself (needs the driver series).
- IWYU / GCC 15 / Clang 21 etc.: no code was touched.

## Findings

### F1. A VM-level stall of 4-11 ms every 5.12 s (testbed, not rocm-ernic)

Symptom: first trial run, one retransmit in a `MSG_ZEROCOPY` transfer. The capture
(`E/trial/default-v4-1to2-capture.txt.gz`, seq 141875266) shows the original 376-byte
tail segment and its tail-loss probe (TS val 6 ms apart) arriving at the same
microsecond: the original was held, not lost.

Cause: stop-and-wait UDP round trips (`E/10-jitter-probe-tx-offloads-1.txt`) are
0.49 ms median with 4-5 outliers of 4-11 ms per 30000, spaced exactly 5.12 s. The
pre-change server (`develop` bd902345) shows the same
(`E/12-jitter-probe-develop-bd902345.txt`), and so does a ping to the SLIRP gateway
over the guest's virtio NIC, which never touches rocm-ernic
(`E/15-stall-attribution-slirp-ping.txt`: 5.8, 8.3, 4.0 ms spikes 5.117 s apart).
So the stall is in the guest or QEMU. I did not find what in them has that period.

Effect: with a 0.5 ms RTT, a stall that long makes TCP send a tail-loss probe or
re-send a FIN. 6 such retransmits in 83 suite runs, every one shown unnecessary
(duplicate in the receiver's capture or DSACK). Not a failure of the device.

### F2. Rx-ring overrun at the receiver under bulk TCP (real loss; new with this build)

Symptom: 3 occurrences. `gso-off:v6` 2->1 on 7.2.15/DKMS (38 retransmits in the
1 GiB transfer), `default:v6` 1->2 on 7.2.15/in-tree (30), and iperf3 `-Z` over IPv6
on 7.4-rc3 KASAN (31, then 485 and 180 in two of four repeats). Hashes matched, but
only thanks to fast retransmits: FAIL by the rule.

Root cause: the receiving guest's Rx ring (1024 descriptors by default) ran out of
posted buffers and the receiver's emulator dropped the frames. In every case
sender retransmits = receiver `hw_rx_dropped` = `hw_rx_over_errors` = emulator
`rx_queue_empty` (`E/k74rc3-intree/iperf3-tcp6-rx-overrun-diagnosis.txt`). No TX
error counter moved; no checksum error.

Confirmed by ring size, 6 x 1 GiB devmem-2 -> devmem-1 each
(`E/16-rx-overrun-vs-ring-size-tx-offloads-1.txt`): `ethtool -G rx 256` drops in 4
of 6 transfers (40, 377, 3, 11 frames, retransmits equal each time); 1024: 0 of 6;
4096: 0 of 6.

**It is new with this build.** The pre-change server at ring 256: 0 drops in 6 GiB
(`E/18-rx-overrun-vs-ring-size-develop-bd902345.txt`). Before, the sending guest's
per-packet cost was the bottleneck; with TSO it hands the device 45 segments per
skb and the sending emulator emits them back to back, faster than the receiving
guest refills. A stall like F1 at the wrong moment is enough at ring 1024 (about
8 ms of frames at 170 MiB/s). I did not separate "F1 stall" from "plain burst" for
the two ring-1024 events.

Verdict: congestion loss at the receiver's ring, as a real NIC with a 1024-entry
ring and a slow receiver has; not a defect in the TX offload code, so nothing was
changed. But it is a user-visible consequence that the CHANGELOG and
`docs/ionic.rst` do not mention: bulk TCP into a slower guest (a debug/KASAN
kernel especially) can now show `hw_rx_dropped` and retransmits. Suggested, for
the owner to decide: (a) a Limitations/Known-issues line with the workaround
`ethtool -G <dev> rx 4096`; (b) have the receive path leave frames in the TAP
when the guest has posted no buffer instead of dropping them (the TAP queue then
absorbs a burst); (c) re-measure `docs/performance.rst`.

Rate at the default ring: 2 events in about 125 one-GiB transfers on the
non-KASAN kernels (104 in suites, the rest in probes and throughput runs). Rerun of each failing suite passed
(`E/k7215-dkms-final-run2.log`). The first run's JSON was overwritten by the
rerun; the FAIL rows are in `results.tsv` and the run logs.

### F3. Harness: guest-wide `TcpRetransSegs` counts sockets that are not under test

`stock726-dkms-v6ext` `default:vlan6`: +1 on devmem-1 while all 47 sender sockets
reported 0 and the receiver's capture had no duplicate. The counter is guest-wide
(ssh sessions drive the test) and the receiver's reply sockets were not
instrumented then. Not attributable after the fact. Fix: retransmits are now read
per test socket, replies included; the guest-wide excess is only noted. Rerun PASS.

### F4. Harness: a duplicated FIN is not DSACKed before the socket is read

7.4-rc3 `ecn:v4`: one retransmit, `dsack_dups` 0, no duplicate data in the
capture. It was the FIN, sent twice 5 ms apart, both copies captured
(`E/k74rc3-intree/ecn-v4-FAIL-duplicate-fin.txt`); same cause as F1. Fix: the
capture analysis counts duplicate FINs; captured-phase retransmits are judged
against the capture. Rerun PASS.

### Observations (no action)

- **IPv4 IDs.** On 7.2.x every packet of a flow, TSO segments included, is +1 from
  the last: about 30500 per capture, 0 out of order, so the per-segment increment
  matches what the stack reserved. v7.4 gives every DF packet ID 0 (RFC 6864), so
  each TSO chain is numbered 0, 1, 2, ... from its template; software GSO on the
  same kernel produces the same pattern (`E/k74rc3-intree/tso-off-v4-*`).
- **CWR** is on the first segment of a TSO burst only (stock 7.2.6, CE marks).
- **Zero UDP checksum**: never on the wire as 0 (no `[no cksum]` in any capture;
  IPv6 would have dropped it).
- **Linearize**: `tx_*_linearize` rises ~400-900 per suite from the 64-byte-piece
  sendfile case (17 frags, non-GSO): the driver's path, data verified.
- **Zerocopy** is really zerocopy now: 0 copied sends; the pre-change server copied
  5664 of 6200 (`E/12-...`).
- **Guest TX packets vs emulator**: e.g. guest `tx_packets` +909511, emulator
  `tx_ucast_packets` +909507 (the rest multicast) in the trial suite.

## Throughput, for interest (7.2.15-devmem, DKMS-source ionic, MTU 1500)

Shared host; run-to-run spread is as large as any difference.

| | pre-change server | tx-offloads-1 |
| --- | --- | --- |
| 1 GiB hashed, MiB/s (1->2 / 2->1), two samples | 240 / 205, 211 / 223 | 174 / 166, 182 / 156 |
| iperf3 TCP 10 s, Mbit/s | 1880 / 1700, 1873 / 1268 | 1387 / 1296, 1579 / 1292 |
| iperf3 `-Z` | 2002 / 1638, 1479 / 1650 | 1368 / 1447, 1572 / 1419 |
| MTU 9000, 1 GiB | not measured | 309-357 |

Files: `E/13-*`, `E/19-*`, `E/k7215-dkms/throughput-tx-offloads-1.txt`,
`E/k7215-dkms-final/throughput-tx-offloads-1-sample2.txt`. At MTU 1500 the new
build was not faster in these samples and was slower in most; inside the matrix
the same transfer ranged 160-250 MiB/s. Not investigated; TSO only pays visibly
under KASAN (200 vs 31 MiB/s).

## Bugs found by testing

None in the emulator, the mainline driver series or the backport. No product file
was changed; `git status` of `~/Projects/rocm-ernic` is identical
to the start of the run. F2 is a behaviour change to document or mitigate, at the
owner's call.

## Testbed changes made (also in `testbed.md` section 10)

- `stack.sh`: the docker network `l2` is now created with MTU 9216 and `up` runs a
  new `jumbo` step (one-off NET_ADMIN container per ernic netns) raising `tap0` and
  `br0` to 9216. Without it guest MTUs above 1500 cannot pass.
- New scripts in `test/scripts/emu-offloads/`: `gt.py` (guest traffic tool),
  `run.py` (matrix driver), `rx-start.sh`, `extras.sh`, `throughput.sh`,
  `jitter-probe.sh`, `rx-overrun-probe.sh`, `cwr-check.sh`.
- Guests: nothing persistent. `/tmp/msg_zerocopy` was built in each (tmpfs).

## State left

Stack running; both guests on `7.2.15-devmem` with the kernel's in-tree ionic (a
restart always returns to it; `ionic-mod.sh load ernic-base` puts the DKMS-source
build back), MTU 1500, Rx ring 1024, all offloads on, server
`/var/tmp/ionic-devmem/test/ernic/tx-offloads-1/build/rocm-ernic`.
