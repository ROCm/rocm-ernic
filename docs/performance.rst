Performance
===========

This page summarises measured RDMA performance for the emulated
device across three milestones.  All tests ran on
**hpe-rack-15.adc.amd.com** (AMD EPYC 7513, 128 threads) with
the TCP mesh backend (manager + worker, 2 guest VMs on
localhost loopback, Ubuntu 24.04 Noble guests, QEMU 10.2.2).

.. contents:: Sections
   :local:
   :depth: 2

Test Environment
----------------

==============================  ==========================================
Component                       Value
==============================  ==========================================
Host kernel                     6.8.0-31-generic (Ubuntu 24.04)
Guest kernel                    6.17.0-19-generic
QEMU                            10.2.2-pci-mmio-bridge-submit
rdma-core                       v62.0 + rocm_ernic provider
Guest RDMA device               rocep1s0 (PVRDMA via vfio-user)
MTU                             4096 bytes
GID index                       1 (IPv4-mapped, 192.168.200.x/24)
Connection type                 RC (Reliable Connected)
==============================  ==========================================


Milestone 1 -- March 25 (``e479b01``)
--------------------------------------

First working RDMA data path.  Only ``ibv_rc_pingpong``
completed; all perftest tools (``ib_send_bw``, ``ib_write_bw``,
``ib_read_bw``, latency variants) hung during connection setup.

**ibv_rc_pingpong** (20 iterations per size):

=========  ================  ==============
Msg Size   Throughput        Latency/iter
=========  ================  ==============
64 B       0.01 Mbit/s       78.0 ms
256 B      0.05 Mbit/s       80.0 ms
1 KB       0.21 Mbit/s       78.0 ms
4 KB       0.84 Mbit/s       78.1 ms
8 KB       1.68 Mbit/s       78.0 ms
=========  ================  ==============

Latency was dominated by a ~41 ms CQ polling interval and a
~78 ms round-trip floor.  ICMP ping over the emulated NIC
showed 62 ms RTT.


Milestone 2 -- March 28 (``54e931d``)
--------------------------------------

All three core verbs working: Send, RDMA Write, RDMA Read.
The CQ polling floor was eliminated, bringing sub-millisecond
latency.  Perftest suite fully functional.  Bidirectional mode
deadlocked at high TX depth.

Bandwidth (GB/s)
^^^^^^^^^^^^^^^^

1000 iterations per data point.  Four independent runs
averaged.  4 KB and 8 KB sizes omitted (startup transient
FAIL).

**Send bandwidth (GB/s):**

=======  ====  ====  ====
Size     Min   Mean  Max
=======  ====  ====  ====
16 KB    0.35  0.38  0.44
32 KB    0.52  0.60  0.71
64 KB    0.88  1.04  1.26
128 KB   1.34  1.50  1.66
256 KB   1.46  1.61  1.76
512 KB   1.14  1.39  1.64
1 MB     0.95  1.20  1.45
4 MB     1.14  1.24  1.35
8 MB     0.97  1.11  1.25
=======  ====  ====  ====

**Write bandwidth (GB/s):**

=======  ====  ====  ====
Size     Min   Mean  Max
=======  ====  ====  ====
16 KB    0.50  0.56  0.62
32 KB    0.83  0.90  0.97
64 KB    1.03  1.14  1.25
128 KB   1.72  1.83  1.94
256 KB   1.77  1.88  1.99
512 KB   1.91  1.95  1.98
1 MB     1.49  1.72  1.96
4 MB     1.88  1.90  1.92
8 MB     1.38  1.43  1.47
=======  ====  ====  ====

**Read bandwidth (GB/s):**

=======  ====  ====  ====
Size     Min   Mean  Max
=======  ====  ====  ====
16 KB    0.55  0.61  0.69
32 KB    1.02  1.11  1.22
64 KB    1.38  1.66  1.76
128 KB   1.86  1.88  1.89
256 KB   1.88  1.91  1.91
512 KB   1.84  1.88  1.92
1 MB     1.87  1.89  1.91
4 MB     1.97  1.98  1.99
8 MB     1.74  1.76  1.77
=======  ====  ====  ====

Latency (us)
^^^^^^^^^^^^

1000 iterations per data point.  Four runs averaged.

**Send latency (us):**

=======  ======  =======  ======
Size     Max     Typical  Min
=======  ======  =======  ======
4 KB     443     316      311
64 KB    471     352      337
256 KB   930     460      453
1 MB     2,250   912      923
8 MB     18,752  6,547    6,608
=======  ======  =======  ======

**Write latency (us):**

=======  ======  =======  ======
Size     Max     Typical  Min
=======  ======  =======  ======
4 KB     231     183      184
64 KB    296     198      199
256 KB   539     317      310
1 MB     1,413   820      819
8 MB     14,682  6,041    6,068
=======  ======  =======  ======

**Read latency (us):**

=======  ======  =======  ======
Size     Max     Typical  Min
=======  ======  =======  ======
4 KB     389     356      354
64 KB    534     356      355
256 KB   721     435      438
1 MB     2,531   839      845
8 MB     10,487  5,131    5,181
=======  ======  =======  ======

Reliability (64 KB, 100 iters x 4 sessions, 8 runs per verb):

- Send: 1.08 -- 1.25 GB/s avg, all 8 runs PASS
- Write: 1.10 -- 1.19 GB/s avg, all 8 runs PASS
- Read: 1.05 -- 1.42 GB/s avg, all 8 runs PASS
- Pingpong: 1156 -- 1336 us/iter, mean ~1260 us


Milestone 3 -- March 29 (``261869c``)
--------------------------------------

Bidirectional deadlock fixed.  Per-direction WQE processing,
in-flight send flow control, doubled ring capacity
(``max_qp_wr`` 500 to 1024), 4 MB TCP socket buffers.
All 24 stress tests pass.

Bandwidth (GB/s)
^^^^^^^^^^^^^^^^

1000 iterations per data point.  Single run.  4 KB and 8 KB
omitted (startup transient FAIL).

=======  ======  ======  ======
Size     Send    Write   Read
=======  ======  ======  ======
16 KB    0.35    0.50    0.55
32 KB    0.46    0.86    1.02
64 KB    1.03    1.10    1.44
128 KB   1.51    1.53    1.95
256 KB   1.73    1.97    1.95
512 KB   1.80    1.84    1.87
1 MB     1.84    1.85    1.87
2 MB     1.84    1.73    1.82
4 MB     1.85    1.73    1.84
8 MB     1.72    1.65    1.51
=======  ======  ======  ======

Latency (us)
^^^^^^^^^^^^

1000 iterations per data point.

**Send latency (us):**

=======  ======  =======  ======
Size     Max     Typical  Min
=======  ======  =======  ======
4 KB     445     273      297
64 KB    470     359      335
256 KB   607     472      472
1 MB     1,224   921      922
8 MB     8,963   5,908    6,046
=======  ======  =======  ======

**Write latency (us):**

=======  ======  =======  ======
Size     Max     Typical  Min
=======  ======  =======  ======
4 KB     289     183      183
64 KB    272     199      200
256 KB   416     304      299
1 MB     1,144   815      815
8 MB     8,661   6,463    6,649
=======  ======  =======  ======

**Read latency (us):**

=======  ======  =======  ======
Size     Max     Typical  Min
=======  ======  =======  ======
4 KB     381     355      354
64 KB    422     357      357
256 KB   689     509      481
1 MB     2,289   841      873
8 MB     12,642  5,652    5,820
=======  ======  =======  ======

Bidirectional Bandwidth (GB/s, combined both directions)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Default ``--tx-depth=100``.  Previously deadlocked.

=======  ======  ======
Size     Send    Write
=======  ======  ======
64 KB    0.98    1.12
256 KB   2.06    2.04
1 MB     2.27    2.35
=======  ======  ======

Reliability (64 KB, 100 iters x 5 runs):

- Send: 0.85 -- 1.02 GB/s avg (min/max across 5 runs)
- Write: 1.04 -- 1.19 GB/s avg
- Read: 1.10 -- 1.35 GB/s avg
- Pingpong: 1119 -- 1246 us/iter, mean ~1195 us

Stress Tests (24/24 PASS):

- Multi-QP (q=2,4,8): Send 1.05--1.14, Write 1.14--1.20 GB/s
- Soak 60s: Send 0.46, Write 1.11 GB/s sustained
- Concurrent send+write 60s: both PASS
- High iteration (1000): Write 1M peaks at 1.91 GB/s
- QP churn: 10/10 cycles
- Resource limits: 64/64 QPs
- iperf3 TCP: 0.10 Mbit/s (rate-limited; ~39 Mbit/s ad hoc)


Milestone Comparison
--------------------

============================  ==============  ==============  ==============
Metric                        Mar 25          Mar 28          Mar 29
============================  ==============  ==============  ==============
Git SHA                       ``e479b01``     ``54e931d``     ``261869c``
Verbs passing                 1 (Send)        3 (S/W/R)       3 (S/W/R)
Iterations tested             20              1000--2000      1000
Send BW @ 64 KB (avg)         N/A             1.04 GB/s       1.03 GB/s
Send BW @ 1 MB (avg)          N/A             1.20 GB/s       1.84 GB/s
Write BW @ 256 KB (avg)       N/A             1.88 GB/s       1.97 GB/s
Read BW @ 128 KB (avg)        N/A             1.88 GB/s       1.95 GB/s
Write lat @ 4 KB (typ)        N/A             183 us          183 us
Send lat @ 4 KB (typ)         N/A             316 us          273 us
Pingpong lat (range)          78,000 us       1,156--1,336    1,119--1,246
Bidir send @ 1 MB             N/A             DEADLOCK        2.27 GB/s
Bidir write @ 1 MB            N/A             DEADLOCK        2.35 GB/s
Stress pass rate              N/A             22/24           24/24
max_qp_wr                     N/A             ~500            1024
iperf3 TCP                    N/A             N/A             0.10 Mbit/s
============================  ==============  ==============  ==============


Known Limitations
-----------------

- **4 KB and 8 KB bandwidth FAILs:** perftest server exits
  before the client connects at very small message sizes.
  Does not affect sizes 16 KB and above.

- **iperf3 rate-limited:** the stress test default
  ``ernic_iperf_bandwidth: 100B/s`` limits the measured
  throughput.  Actual achievable TCP throughput over the
  emulated NIC is approximately 39 Mbit/s.

- **Host sysctl required:** TCP socket buffer increase needs
  ``net.core.wmem_max`` and ``net.core.rmem_max`` set to at
  least 16 MB on the host for large-message bidirectional
  traffic.
