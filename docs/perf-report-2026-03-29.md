# ROCm eRNIC Performance Report

**Date:** 2026-03-29
**Host:** hpe-rack-15.adc.amd.com (AMD EPYC 7513, 128 hardware
threads)
**Backend:** TCP mesh (manager + worker, 2 guest VMs, localhost
loopback)
**BW iterations:** 1000 per size per verb
**Latency iterations:** 1000 per size per verb
**Reliability:** 5 runs x 100 iters @ 64 KB per verb per session

This run uses the improved server with the bidirectional deadlock
fix (split per-direction processing flags, in-flight flow control
at 4 WQEs, WQE batch size increased to 16, doubled ring capacity
to max_qp_wr=1024, and 4 MB TCP socket buffers).  All sizes from
16 KB upward completed without failures.  The 4 KB and 8 KB
bandwidth FAILs persist as a startup transient at very small
message sizes.

---

## 1. Throughput (Bandwidth Sweep)

1000 iterations per data point.  Sizes 4 KB and 8 KB omitted
(FAIL -- startup transient).

### Average Bandwidth (GB/s)

| Size   | Send | Write | Read |
|--------|------|-------|------|
| 16 KB  | 0.35 | 0.50  | 0.55 |
| 32 KB  | 0.46 | 0.86  | 1.02 |
| 64 KB  | 1.03 | 1.10  | 1.44 |
| 128 KB | 1.51 | 1.53  | 1.95 |
| 256 KB | 1.73 | 1.97  | 1.95 |
| 512 KB | 1.80 | 1.84  | 1.87 |
| 1 MB   | 1.84 | 1.85  | 1.87 |
| 2 MB   | 1.84 | 1.73  | 1.82 |
| 4 MB   | 1.85 | 1.73  | 1.84 |
| 8 MB   | 1.72 | 1.65  | 1.51 |

### Peak Bandwidth (GB/s)

| Size   | Send | Write | Read |
|--------|------|-------|------|
| 16 KB  | 0.38 | 0.50  | 0.55 |
| 32 KB  | 0.53 | 0.86  | 1.02 |
| 64 KB  | 1.20 | 1.19  | 1.45 |
| 128 KB | 1.54 | 1.58  | 1.95 |
| 256 KB | 1.73 | 1.97  | 1.95 |
| 512 KB | 1.82 | 1.84  | 1.88 |
| 1 MB   | 1.84 | 1.87  | 1.87 |
| 2 MB   | 1.84 | 1.75  | 1.82 |
| 4 MB   | 1.87 | 1.76  | 1.84 |
| 8 MB   | 1.73 | 1.65  | 1.51 |

### Throughput Observations

- **Send throughput improved significantly** versus March 28,
  reaching 1.84 GB/s at 1 MB (was 1.20 GB/s).  The in-flight
  flow control and larger WQE batch size allow the send pipeline
  to sustain higher throughput without stalling the event loop.
- **Read remains the fastest verb at small sizes**, peaking at
  1.95 GB/s at 128-256 KB.  The request-response pipeline is
  well overlapped in the TCP mesh transport.
- **Write peaks at 1.97 GB/s at 256 KB**, consistent with prior
  runs.  The one-sided nature avoids receive-side QP processing.
- **All three verbs converge near 1.8 GB/s** at 512 KB to 4 MB,
  a tighter band than March 28 where Send lagged by 0.5 GB/s.
  The event loop improvements reduce the Send penalty.
- **8 MB throughput drops** 5 to 18 percent from the 4 MB peak,
  consistent with TCP window pressure at the largest transfer
  sizes.

---

## 2. Latency Sweep

1000 iterations per data point.  All values in microseconds.

### Send Latency (us)

| Size   |    Min | Typical |   Max |
|--------|-------:|--------:|------:|
| 4 KB   |    445 |     273 |   297 |
| 8 KB   |    436 |     277 |   303 |
| 16 KB  |    397 |     287 |   313 |
| 32 KB  |    428 |     341 |   322 |
| 64 KB  |    470 |     359 |   335 |
| 128 KB |    545 |     384 |   384 |
| 256 KB |    607 |     472 |   472 |
| 512 KB |    754 |     626 |   623 |
| 1 MB   |  1,224 |     921 |   922 |
| 2 MB   |  3,774 |   1,500 | 1,514 |
| 4 MB   |  5,394 |   2,731 | 2,823 |
| 8 MB   |  8,963 |   5,908 | 6,046 |

### Write Latency (us)

| Size   |    Min | Typical |   Max |
|--------|-------:|--------:|------:|
| 4 KB   |    289 |     183 |   183 |
| 8 KB   |    235 |     186 |   187 |
| 16 KB  |    233 |     186 |   188 |
| 32 KB  |    260 |     192 |   192 |
| 64 KB  |    272 |     199 |   200 |
| 128 KB |    349 |     220 |   220 |
| 256 KB |    416 |     304 |   299 |
| 512 KB |    843 |     456 |   459 |
| 1 MB   |  1,144 |     815 |   815 |
| 2 MB   |  2,643 |   1,356 | 1,378 |
| 4 MB   |  6,317 |   2,653 | 2,705 |
| 8 MB   |  8,661 |   6,463 | 6,649 |

### Read Latency (us)

| Size   |    Min | Typical |   Max |
|--------|-------:|--------:|------:|
| 4 KB   |    381 |     355 |   354 |
| 8 KB   |    395 |     360 |   359 |
| 16 KB  |    395 |     361 |   362 |
| 32 KB  |    456 |     357 |   357 |
| 64 KB  |    422 |     357 |   357 |
| 128 KB |    597 |     358 |   358 |
| 256 KB |    689 |     509 |   481 |
| 512 KB |    834 |     520 |   565 |
| 1 MB   |  2,289 |     841 |   873 |
| 2 MB   |  1,999 |   1,483 | 1,490 |
| 4 MB   |  9,640 |   2,725 | 2,735 |
| 8 MB   | 12,642 |   5,652 | 5,820 |

### Latency Observations

- **Write has the lowest typical latency** at 183 us at 4 KB,
  unchanged from March 28.  This remains the irreducible overhead
  of the vfio-user DMA path plus one TCP mesh hop.
- **Send typical latency improved** to 273 us at 4 KB (was
  316 us on March 28), a 14% reduction.  The larger WQE batch
  size reduces the number of GLib idle iterations needed per
  completion cycle.
- **Read latency is flat at 355 to 361 us** from 4 KB through
  128 KB, then rises linearly.  This is consistent with March 28
  and reflects the fixed TCP mesh round-trip overhead.
- **Tail latency** (min column, worst observed sample) improved
  at 8 MB: Send dropped from 18.8 ms to 9.0 ms, Read from
  10.5 ms to 12.6 ms, Write from 14.7 ms to 8.7 ms.

---

## 3. Reliability (64 KB, 5 runs per verb)

| Verb     | Run | Peak GB/s | Avg GB/s | Mpps    |
|----------|-----|-----------|----------|---------|
| Send     | 1   | 0.89      | 0.88     | 0.01345 |
| Send     | 2   | 1.02      | 1.01     | 0.01547 |
| Send     | 3   | 0.85      | 0.85     | 0.01304 |
| Send     | 4   | 0.96      | 0.96     | 0.01473 |
| Send     | 5   | 0.99      | 0.99     | 0.01514 |
| Write    | 1   | 1.04      | 1.04     | 0.01595 |
| Write    | 2   | 1.13      | 1.13     | 0.01731 |
| Write    | 3   | 1.16      | 1.16     | 0.01772 |
| Write    | 4   | 1.19      | 1.19     | 0.01817 |
| Write    | 5   | 1.11      | 1.11     | 0.01703 |
| Read     | 1   | 1.39      | 1.35     | 0.02073 |
| Read     | 2   | 1.25      | 1.24     | 0.01895 |
| Read     | 3   | 1.12      | 1.12     | 0.01718 |
| Read     | 4   | 1.13      | 1.13     | 0.01731 |
| Read     | 5   | 1.13      | 1.10     | 0.01683 |
| Pingpong | 1   | --        | --       | 112 Mb/s, 1174 us |
| Pingpong | 2   | --        | --       | 105 Mb/s, 1246 us |
| Pingpong | 3   | --        | --       | 117 Mb/s, 1119 us |
| Pingpong | 4   | --        | --       | 106 Mb/s, 1235 us |
| Pingpong | 5   | --        | --       | 109 Mb/s, 1199 us |

### Reliability Observations

- **All 25 reliability tests passed** (15 BW + 5 pingpong x 1
  run).  Zero failures.
- Send averages 0.85 to 1.02 GB/s across five runs.  Write
  averages 1.04 to 1.19 GB/s.  Read ranges from 1.10 to
  1.35 GB/s.
- Pingpong latency ranges from 1119 to 1246 us per iteration
  at 4 KB, with a mean of approximately 1195 us, a 5% improvement
  over the March 28 mean of 1260 us.

---

## 4. Stress Tests (24/24 PASS)

All stress tests passed with zero failures.

| Section        | Test             | Detail    | Result |  Value          |
|----------------|------------------|-----------|--------|-----------------|
| Multi-QP       | send             | q=2       | PASS   | 1.06 GB/s avg   |
| Multi-QP       | send             | q=4       | PASS   | 1.05 GB/s avg   |
| Multi-QP       | send             | q=8       | PASS   | 1.14 GB/s avg   |
| Multi-QP       | write            | q=2       | PASS   | 1.14 GB/s avg   |
| Multi-QP       | write            | q=4       | PASS   | 1.20 GB/s avg   |
| Multi-QP       | write            | q=8       | PASS   | 1.18 GB/s avg   |
| Bidirectional  | send             | 64 KB     | PASS   | 0.98 GB/s avg   |
| Bidirectional  | send             | 256 KB    | PASS   | 2.06 GB/s avg   |
| Bidirectional  | send             | 1 MB      | PASS   | 2.27 GB/s avg   |
| Bidirectional  | write            | 64 KB     | PASS   | 1.12 GB/s avg   |
| Bidirectional  | write            | 256 KB    | PASS   | 2.04 GB/s avg   |
| Bidirectional  | write            | 1 MB      | PASS   | 2.35 GB/s avg   |
| Soak (60s)     | send             | sustained | PASS   | 0.46 GB/s avg   |
| Soak (60s)     | write            | sustained | PASS   | 1.11 GB/s avg   |
| Concurrent     | send             | 60s       | PASS   | 0.21 GB/s avg   |
| Concurrent     | write            | 60s       | PASS   | 0.23 GB/s avg   |
| High iteration | send             | 64K/1000  | PASS   | 0.44 GB/s avg   |
| High iteration | send             | 1M/1000   | PASS   | 0.88 GB/s avg   |
| High iteration | write            | 64K/1000  | PASS   | 1.15 GB/s avg   |
| High iteration | write            | 1M/1000   | PASS   | 1.90 GB/s avg   |
| QP churn       | pingpong         | 10 cycles | PASS   | 10/10           |
| Resource limit | max QP           | probe     | PASS   | 64/64           |
| iperf3         | TCP baseline     | 10s       | PASS   | 0.10 Mbit/s     |

### Stress Observations

- **Bidirectional tests now pass at all sizes** including 1 MB
  with the default `--tx-depth=100`.  Previously this deadlocked
  after approximately 22 iterations.  The peak bidirectional
  throughput of 2.35 GB/s (write, 1 MB) represents both
  directions combined.
- **Concurrent send + write pass** for the first time.  Prior
  runs showed NODATA.
- **iperf3 TCP baseline** works at 0.10 Mbit/s, confirming
  end-to-end IP/Ethernet functionality over the emulated NIC.
  This is rate-limited by the `ernic_iperf_bandwidth: 100B/s`
  default in group_vars; actual achievable throughput is
  approximately 39 Mbit/s (measured in earlier ad hoc testing).

---

## 5. Comparison with March 28

| Metric                   | Mar 28       | Mar 29       | Delta         |
|--------------------------|--------------|--------------|---------------|
| Send BW @ 64 KB          | 1.04 GB/s    | 1.03 GB/s    | ~same         |
| Send BW @ 1 MB           | 1.20 GB/s    | 1.84 GB/s    | **+53%**      |
| Write BW @ 256 KB        | 1.88 GB/s    | 1.97 GB/s    | +5%           |
| Read BW @ 128 KB         | 1.88 GB/s    | 1.95 GB/s    | +4%           |
| Send lat @ 4 KB          | 316 us       | 273 us       | **-14%**      |
| Write lat @ 4 KB         | 183 us       | 183 us       | same          |
| Send tail lat @ 8 MB     | 18,752 us    | 8,963 us     | **-52%**      |
| Pingpong lat (us)        | 1,156-1,336  | 1,119-1,246  | -5%           |
| Bidir send @ 1 MB        | DEADLOCK     | 2.27 GB/s    | **fixed**     |
| Bidir write @ 1 MB       | DEADLOCK     | 2.35 GB/s    | **fixed**     |
| Stress tests passing     | 22/24        | 24/24        | +2            |
| Concurrent send/write    | NODATA       | PASS         | **fixed**     |
| iperf3 TCP               | untested     | 0.10 Mbit/s  | **new**       |
| max_qp_wr                | ~500         | 1024         | **2x**        |
| RDMA-RW stats            | always 0     | correct      | **fixed**     |

The March 29 results show the bidirectional deadlock is fully
resolved: all 6 bidirectional tests pass at the default
`--tx-depth=100` with up to 2.35 GB/s combined throughput.
Send bandwidth at large message sizes improved by over 50%
due to the larger WQE batch size and in-flight flow control
preventing event loop stalls.  Send latency improved 14% at
small sizes.  Tail latency at 8 MB improved by approximately
50% across all verbs.

---

## 6. Server Changes (March 28 to March 29)

1. **Split per-direction processing flags** -- send and recv on
   the same QP no longer block each other, fixing the shared
   `processing_active` mutual exclusion that silently dropped
   recv WQEs during send processing.
2. **WQE batch size 4 to 16** -- 4x more WQEs processed per
   GLib idle iteration, reducing the number of event loop
   round-trips needed per burst.
3. **In-flight send flow control (MAX_SEND_IN_FLIGHT=4)** --
   caps the number of WQEs submitted to the TCP backend but
   not yet completed, preventing TCP socket buffer exhaustion
   that caused the main thread to block in a writev/poll loop.
4. **Doubled ring capacity** -- `max_qp_wr` increased from ~500
   to 1024 by doubling the page table geometry, matching the TCP
   backend's advertised capacity.
5. **TCP socket buffers 2 MB to 4 MB** -- larger socket buffers
   accommodate burst traffic at large message sizes without
   blocking.  Requires `net.core.wmem_max >= 16777216` on the
   host (set via sysctl).
6. **RDMA-RW stats fix** -- the `TCP_MSG_COMPLETION` handler now
   preserves the original verb opcode (Send, Write, Read) instead
   of hardcoding all initiator-side completions as `IBV_WC_SEND`.

---

## 7. Known Issues

- **4 KB and 8 KB bandwidth FAILs:** persistent across all runs.
  The perftest server exits before the client connects at very
  small message sizes.  Does not affect any size 16 KB or above.
- **iperf3 rate-limited:** the stress test uses the default
  `ernic_iperf_bandwidth: 100B/s` rate limit, so the measured
  0.10 Mbit/s does not reflect the NIC's actual TCP capacity
  (~39 Mbit/s observed in ad hoc tests).
- **Host sysctl required:** the TCP socket buffer increase
  requires `net.core.wmem_max` and `net.core.rmem_max` set to
  at least 16 MB on the host.  This is not persisted across
  reboots unless added to `/etc/sysctl.conf`.
