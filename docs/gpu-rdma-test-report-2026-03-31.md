# GPU Direct RDMA Test Report

Date: 2026-03-31
Host: hpe-rack-15.adc.amd.com

## Test Environment

| Component               | Detail                                 |
|-------------------------|----------------------------------------|
| Host OS                 | Ubuntu 24.04.4 LTS                     |
| Guest kernel            | 6.17.0-19-generic                      |
| GPU                     | AMD Instinct MI210 (0x740f rev 0x02)   |
| RDMA device             | rocep2s0 (vendor=0x1022, part=0x8000)  |
| RDMA transport          | InfiniBand, PORT_ACTIVE, MTU 4096      |
| QEMU                    | 10.2.2-pci-mmio-bridge-submit          |
| ROCm                    | 7.2.1                                  |
| rocm-xio                | main (pci-mmio-bridge merged)          |
| GPU passthrough         | vfio-pci + pci-mmio-bridge             |
| VM config               | 8 vCPUs, 16 GB RAM, KVM               |
| Instances               | 2 VMs (manager + worker)               |

## Test Configurations

### GPU DV Loopback (xio-tester)

GPU-initiated RDMA Write via the Direct Verbs path. The
GPU writes WQEs, updates shared ring state, and rings the
doorbell through pci-mmio-bridge. The emulated device
server processes the WQE and posts a completion to the CQ
ring, which the GPU polls.

- **Provider**: rocm-ernic (emulated ERNIC via vfio-user)
- **Mode**: single-VM loopback (QP sends to itself)
- **Doorbell**: pci-mmio-bridge command ring -> BAR2 UAR
- **SQ/CQ buffers**: system memory (posix_memalign +
  hipHostRegister)
- **Data buffers**: system memory (posix_memalign)
- **SQ depth**: 256, WQE size 128B, header page offset

### CPU 2-Node (perftest)

Standard verbs RDMA Write between VM1 and VM2 using
ib_write_bw and ib_write_lat from perftest. The kernel
driver handles WQE posting and CQ polling. Included as
a baseline comparison for the emulated device.

- **Provider**: rocm-ernic (standard ibverbs path)
- **Mode**: VM1 (server) <-> VM2 (client) over TCP mesh
- **Data buffers**: CPU memory (ibv_reg_mr)

## Results

### GPU DV Loopback -- Iteration Sweep (size=4096B)

| VM  | Iters | Status | Min (us) | Avg (us) | Max (us) |
|-----|-------|--------|----------|----------|----------|
| VM1 |     1 | PASS   |    838.1 |    838.1 |    838.1 |
| VM1 |    10 | PASS   |    677.8 |   1002.7 |   1157.8 |
| VM1 |   100 | PASS   |    333.0 |   1019.6 |   1169.4 |
| VM2 |     1 | PASS   |    387.8 |    387.8 |    387.8 |
| VM2 |    10 | PASS   |    195.0 |    947.4 |   1144.8 |
| VM2 |   100 | PASS   |    808.6 |   1025.6 |   1177.6 |

### GPU DV Loopback -- Transfer Size Sweep (iters=10)

| VM  |  Size | Status | Min (us) | Avg (us) | Max (us) |
|-----|-------|--------|----------|----------|----------|
| VM1 |    64 | PASS   |    982.1 |   1032.4 |   1145.4 |
| VM1 |   256 | PASS   |    402.2 |    970.3 |   1149.3 |
| VM1 |  1024 | PASS   |    617.1 |    986.3 |   1145.8 |
| VM1 |  4096 | PASS   |    186.9 |    945.9 |   1141.6 |
| VM1 | 16384 | PASS   |    575.0 |    986.2 |   1151.2 |
| VM1 | 65536 | FAIL   |       -- |       -- |       -- |
| VM2 |    64 | PASS   |    464.6 |    976.3 |   1148.8 |
| VM2 |   256 | PASS   |    694.4 |    979.2 |   1142.9 |
| VM2 |  1024 | PASS   |    455.8 |    978.6 |   1160.2 |
| VM2 |  4096 | PASS   |    315.2 |    948.0 |   1165.6 |
| VM2 | 16384 | PASS   |    254.2 |    959.9 |   1145.4 |
| VM2 | 65536 | FAIL   |       -- |       -- |       -- |

### CPU 2-Node Baseline (perftest, size=4096B)

| Test          | BW (MB/s) | Lat min (us) | Lat avg (us) | Lat max (us) | Lat p99 (us) |
|---------------|-----------|--------------|--------------|--------------|--------------|
| ib_write_bw   |    790.42 |           -- |           -- |           -- |           -- |
| ib_write_lat  |        -- |       174.50 |       181.15 |       208.49 |       208.49 |

### Summary

- **16/18 GPU DV loopback tests pass** across both VMs
- GPU DV loopback average latency: ~1 ms per RDMA Write
  (dominated by pci-mmio-bridge 1 ms poll interval and
  vfio-user DMA mapping overhead)
- CPU 2-node write latency: ~181 us average (5.6x faster
  than GPU DV loopback, as expected since CPU path avoids
  pci-mmio-bridge polling and GPU kernel launch overhead)
- GPU DV latency is **flat across transfer sizes 64B to
  16 KB**, confirming overhead is in the doorbell and
  completion path, not data copy
- Both VMs produce consistent results
- CPU 2-node bandwidth: 790 MB/s at 4 KB (limited by
  TCP mesh backend, not hardware)

### Failures

- **65536B GPU transfers**: `illegal memory access` --
  the data buffer is registered with `2 * transfer_size`
  bytes (8192B for 4 KB transfers). 65536B requires
  131072B but the MR registration only covers 8192B.
  This is a rocm-xio buffer sizing issue.

## GPU Direct RDMA Data Path (verified)

```
GPU Kernel
    |
    |  1. Write WQE to SQ buffer (host mem, PAGE_SIZE offset)
    |  2. Update ring_state prod_tail (shared page)
    |  3. genPciMmioBridgeCmd(shadow, BDF, BAR2, offset, val)
    v
pci-mmio-bridge (QEMU device, polls shadow GPA)
    |
    |  4. Forward write to ERNIC BAR2 (secondary bus BDF)
    v
rocm-ernic server (vfio-user, BAR2 callback)
    |
    |  5. pvrdma_qp_send -> read WQE from page directory
    |  6. Local loopback: memcpy data to target MR
    |  7. Write CQE to CQ ring, update ring_state[1] prod_tail
    v
GPU Kernel
    |
    |  8. poll_cq_until: read CQ ring_state[1] prod/cons
    |  9. Detect completion, advance cons_head and sq_head
    | 10. Return from quiet(), iteration complete
    v
Next iteration
```

## Known Limitations

- **CQ/SQ in system memory only**: GPU VRAM buffers
  require vfio-user DMA proxy for GPU BAR regions
  (QEMU has full GPA visibility but the vfio-user
  server does not). Tracked for future work.
- **GPU DV loopback only**: 2-node GPU RDMA (VM1 GPU
  -> VM2 GPU) requires xio-tester 2-node mode and
  cross-node MR resolution in the TCP backend.
- **~1 ms latency floor**: dominated by pci-mmio-bridge
  poll interval (1 ms default). Reducing
  `poll-interval-ns` in the QEMU command line would
  lower latency at the cost of CPU usage.
- **Performance vs real hardware**: the emulated path
  adds vfio-user IPC, DMA mapping, and bridge polling
  overhead. Real ERNIC hardware eliminates all of
  these, targeting sub-microsecond latency.

## Next Steps

1. Reduce pci-mmio-bridge poll interval and re-measure
   GPU DV latency sensitivity
2. Implement 2-node GPU RDMA in xio-tester (connect
   VM1 QP to VM2 QP via the TCP mesh)
3. Investigate GPU VRAM CQ/SQ via vfio-user DMA proxy
4. Fix 65536B+ transfer sizing in rocm-xio
5. Profile and optimize the server completion path
