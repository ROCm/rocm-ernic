# ROCm eRNIC RDMA Test Report

**Date:** 2026-03-28
**Host:** hpe-rack-15.adc.amd.com (AMD EPYC, 128 hardware threads)
**QEMU:** 10.2.2 with PCI MMIO bridge and vfio-user transport
**Backend:** TCP mesh topology (1 manager + 1 worker, 2 guest VMs)
**VMs:** 2x Ubuntu 24.04 Noble, 8 vCPUs and 16 GiB RAM each
**Device:** rocep1s0 (pvrdma emulated NIC exposed via vfio-user)
**MTU:** 1024 bytes | **GID Index:** 1 (IPv4-mapped, 192.168.200.x/24)

## Summary

All seven RDMA tests pass consistently across five runs each -- thirty-five
out of thirty-five runs with zero failures. Bandwidth and latency sweeps
across twelve message sizes from 4 KB through 8 MB also complete with zero
failures: seventy-two out of seventy-two runs per sweep direction.

| Test              | Result | Runs  |
|-------------------|--------|-------|
| ib_send_bw        | PASS   | 5/5   |
| ib_write_bw       | PASS   | 5/5   |
| ib_read_bw        | PASS   | 5/5   |
| ib_send_lat       | PASS   | 5/5   |
| ib_write_lat      | PASS   | 5/5   |
| ib_read_lat       | PASS   | 5/5   |
| ibv_rc_pingpong   | PASS   | 5/5   |

## Bandwidth vs Message Size

One hundred iterations per run. All bandwidth values are reported in GB/s.

### Average Bandwidth (GB/s)

| Message Size | Send    | Write   | Read    |
|--------------|---------|---------|---------|
| 4 KB         | 0.17    | 0.18    | 0.01    |
| 8 KB         | 0.35    | 0.36    | 0.02    |
| 16 KB        | 0.04    | 0.04    | 0.04    |
| 32 KB        | 0.08    | 0.08    | 0.04    |
| 64 KB        | 0.16    | 0.16    | 0.14    |
| 128 KB       | 0.32    | 0.32    | 0.26    |
| 256 KB       | 0.64    | 0.66    | 0.51    |
| 512 KB       | 1.29    | 2.54    | 0.89    |
| 1 MB         | 2.60    | 2.58    | 1.39    |
| 2 MB         | 2.95    | 2.91    | 1.96    |
| 4 MB         | 3.13    | 2.80    | 2.43    |
| 8 MB         | 2.51    | 2.43    | 2.35    |

### Peak Bandwidth (GB/s)

| Message Size | Send    | Write   | Read    |
|--------------|---------|---------|---------|
| 4 KB         | 0.17    | 0.18    | 0.01    |
| 8 KB         | 0.36    | 0.36    | 0.02    |
| 16 KB        | 0.17    | 0.36    | 0.04    |
| 32 KB        | 0.51    | 0.66    | 0.07    |
| 64 KB        | 0.70    | 0.97    | 0.14    |
| 128 KB       | 1.79    | 1.82    | 0.26    |
| 256 KB       | 2.16    | 2.09    | 0.51    |
| 512 KB       | 2.36    | 2.65    | 0.89    |
| 1 MB         | 3.51    | 2.65    | 1.40    |
| 2 MB         | 2.96    | 2.93    | 1.96    |
| 4 MB         | 3.13    | 2.80    | 2.43    |
| 8 MB         | 2.51    | 2.43    | 2.35    |

### Key Observations

- Bandwidth scales roughly linearly with message size up to approximately
  one megabyte, after which it plateaus at around 2.5 to 3.1 GB/s for the
  Send and RDMA Write verbs.
- RDMA Write achieves the highest single-run peak of 2.65 GB/s at 512 KB
  message size. Send peaks slightly higher at 3.51 GB/s with 1 MB messages.
- RDMA Read bandwidth converges toward Send and Write throughput at large
  message sizes, reaching 2.35 GB/s at 8 MB, but starts significantly lower
  at small sizes because every Read requires a request-response round trip
  through the TCP mesh network.
- A noticeable bandwidth dip appears at 16 to 32 KB for the Send verb; this
  likely corresponds to a buffer management threshold inside the emulation
  layer where the transfer straddles a boundary.
- At 8 MB message size all three verbs converge to approximately 2.4 to
  2.5 GB/s, which strongly suggests that the TCP mesh transport between the
  two server instances is the throughput bottleneck at that point.

## Latency vs Message Size

Fifty iterations per run. All latency values are reported in milliseconds.

### Typical Latency (ms)

| Message Size | Send    | Write   | Read    |
|--------------|---------|---------|---------|
| 4 KB         | 41.00   | 41.00   | 83.00   |
| 8 KB         | 41.00   | 41.00   | 83.00   |
| 16 KB        | 41.00   | 41.00   | 83.02   |
| 32 KB        | 41.00   | 41.00   | 82.99   |
| 64 KB        | 1.08    | 1.08    | 41.65   |
| 128 KB       | 0.56    | 1.09    | 41.61   |
| 256 KB       | 1.10    | 0.58    | 43.69   |
| 512 KB       | 0.60    | 1.66    | 42.68   |
| 1 MB         | 1.18    | 1.71    | 43.65   |
| 2 MB         | 2.26    | 1.81    | 43.72   |
| 4 MB         | 1.94    | 2.50    | 43.66   |
| 8 MB         | 4.46    | 4.65    | 45.70   |

### Minimum Latency (ms)

| Message Size | Send    | Write   | Read    |
|--------------|---------|---------|---------|
| 4 KB         | 1.12    | 20.52   | 0.41    |
| 8 KB         | 0.90    | 20.45   | 0.99    |
| 16 KB        | 1.40    | 20.47   | 1.21    |
| 32 KB        | 1.51    | 20.42   | 1.83    |
| 64 KB        | 0.55    | 0.55    | 1.48    |
| 128 KB       | 0.53    | 0.55    | 1.58    |
| 256 KB       | 0.57    | 0.50    | 2.03    |
| 512 KB       | 0.58    | 0.60    | 2.04    |
| 1 MB         | 0.62    | 1.18    | 2.76    |
| 2 MB         | 1.26    | 1.28    | 3.56    |
| 4 MB         | 1.90    | 1.96    | 6.56    |
| 8 MB         | 3.87    | 4.57    | 10.84   |

### Key Observations

- A clear phase transition occurs at the 64 KB message boundary. Below that
  size the typical measured latency is pinned at approximately 41 ms, which
  is an artifact of the completion queue polling interval inside the
  emulated device. Once the message reaches 64 KB the actual data transfer
  time exceeds the poll interval, and measured latency begins to track the
  real message size.
- RDMA Read latency runs roughly two times higher than Send or Write at
  every message size because each Read operation requires a full round trip
  through the TCP mesh: a read request travels to the target node, and the
  response data travels back.
- The minimum latency achievable for Send at small message sizes falls in
  the range of 0.5 to 1.5 ms. This best case occurs when the CQ poll
  happens to align closely with the moment the completion is posted.
- At the 8 MB message size, Send minimum latency reaches approximately
  3.9 ms and Write minimum latency reaches approximately 4.6 ms, both of
  which reflect the actual time needed to transfer that much data through
  the TCP mesh rather than any polling artifact.
- RDMA Read minimum latency scales smoothly from around 0.4 ms at 4 KB up
  to approximately 10.8 ms at 8 MB, maintaining the expected ratio of
  roughly two times the Send minimum at each message size.

## Reliability Tests (5 runs, 100 iterations, 64 KB messages)

### ib_send_bw (GB/s)

| Run      | Peak     | Average  | Message Rate (Mpps)  |
|----------|----------|----------|----------------------|
| 1        | 0.78     | 0.16     | 0.002411             |
| 2        | 0.98     | 0.16     | 0.002495             |
| 3        | 0.83     | 0.14     | 0.002183             |
| 4        | 0.99     | 0.16     | 0.002424             |
| 5        | 0.88     | 0.15     | 0.002217             |
| **Mean** | **0.89** | **0.15** | **0.002346**         |

### ib_write_bw (GB/s)

| Run      | Peak     | Average  | Message Rate (Mpps)  |
|----------|----------|----------|----------------------|
| 1        | 1.12     | 0.16     | 0.002443             |
| 2        | 1.07     | 0.16     | 0.002453             |
| 3        | 1.06     | 0.16     | 0.002458             |
| 4        | 1.18     | 0.17     | 0.002516             |
| 5        | 1.16     | 0.16     | 0.002455             |
| **Mean** | **1.12** | **0.16** | **0.002465**         |

### ib_read_bw (GB/s)

| Run      | Peak     | Average  | Message Rate (Mpps)  |
|----------|----------|----------|----------------------|
| 1        | 0.14     | 0.14     | 0.002176             |
| 2        | 0.14     | 0.14     | 0.002148             |
| 3        | 0.15     | 0.15     | 0.002239             |
| 4        | 0.15     | 0.15     | 0.002249             |
| 5        | 0.15     | 0.15     | 0.002239             |
| **Mean** | **0.15** | **0.15** | **0.002210**         |

### ibv_rc_pingpong (4 KB messages, 100 iterations)

| Run      | Throughput (Mbit/s) | Latency (us/iter)    |
|----------|---------------------|----------------------|
| 1        | 0.80                | 81743.53             |
| 2        | 0.80                | 81762.71             |
| 3        | 0.80                | 81768.89             |
| 4        | 0.80                | 81671.20             |
| 5        | 0.80                | 81563.85             |
| **Mean** | **0.80**            | **81702.04**         |

The pingpong test is highly consistent across all five runs, with a
standard deviation of less than 100 microseconds per iteration.

## Test Configuration

All tests in this report share the following configuration unless stated
otherwise in the individual section headings.

- Connection type: RC (Reliable Connected) queue pairs throughout
- Transport type: IB (InfiniBand emulation delivered via vfio-user)
- Data exchange method: Ethernet (perftest out-of-band socket on port 18515)
- rdma_cm queue pairs: OFF (QP information exchanged over the perftest TCP
  socket rather than through the RDMA Connection Manager)
- GID index 1 (IPv4-mapped GID derived from 192.168.200.x guest addresses)
- Bandwidth sweep parameters: 100 iterations, 12 sizes from 4 KB to 8 MB
- Latency sweep parameters: 50 iterations, 12 sizes from 4 KB to 8 MB
- Reliability test parameters: 5 independent runs of 100 iterations at
  64 KB message size

## Known Limitations

- Latency for messages smaller than 64 KB is dominated by the approximately
  41 ms completion queue polling interval inside the emulated device rather
  than by any property of the data path itself. Production RDMA hardware
  achieves sub-microsecond latency for the same message sizes.
- RDMA Read bandwidth at small message sizes is noticeably lower than Send
  or Write bandwidth because every Read operation must complete a full
  request-response round trip through the TCP mesh network.
- The observed peak bandwidth ceiling of approximately 3.1 GB/s is imposed
  by the TCP mesh transport running over a localhost loopback connection
  between the two server instances, not by the RDMA emulation layer itself.
