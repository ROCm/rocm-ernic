# Stack 3 (`ionic-devmem-r102`): GPU-to-GPU devmem TCP, end to end

2026-11-20, 16:40Z to 17:30Z. `B` = `/var/tmp/ionic-devmem/test-rocm102`, `O` =
`/var/tmp/ionic-devmem/overnight2/e2e`, `E` = `O/evidence`. Everything below was RUN by me
unless marked **NOT RUN**. Nothing was committed or posted; rocm-ernic, qemu-minimal and
stacks 1 and 2 were not touched. Reads on from [rocm102-gpu.md](rocm102-gpu.md) (devmem TX
from GPU buffers), [rocm102-p2p-kernel.md](rocm102-p2p-kernel.md) (the `7.2.6-devmemp2p`
kernel carrying 69ab87acc614), [rocm102-rx.md](rocm102-rx.md) (devmem RX bring-up) and
[rocm102-frag-limit.md](rocm102-frag-limit.md) (the `rxfrag` module set).

## 1. Result

**GPU-to-GPU devmem TCP works end to end on this stack, in both directions of the GPU
boundary.** The NIC emulator reads the sender's VRAM and *writes into the receiver's VRAM*;
the payload is verified at the receiver through the GPU path (GEM mapping, `hipMemcpy`, or a
GPU kernel), and independently on the host by hashing the receiving NIC server's own mapping
of the GPU's BAR0 at the address the driver gave the device.

| Case | Result |
| --- | --- |
| **Gating check**: is the receiving server's VRAM mapping writable? | **Yes, not a blocker.** Section 2 |
| a. udmabuf TX -> VRAM (GEM) RX | **PASS**, 64 MiB, placement proven both ends, host hash equal |
| b. **VRAM (GEM) TX -> VRAM (GEM) RX** (headline) | **PASS**, 64 MiB, all TX frags and all RX payload descriptors in GPU BAR0, host hash equal |
| c. `hipMalloc` TX -> `hipMalloc` RX, verified by HIP D2H | **PASS**, 32 MiB; and by a **GPU kernel** reading the NIC's bytes, **PASS** at 8 MiB, **hung** at 32 MiB (finding F1) |
| d. Sizes 4 MiB..**1020 MiB** (GEM) and 16..**960 MiB** (`hipMalloc`), repeated | **PASS** everywhere. **1 MiB fails** — page-pool starvation, finding F2 |
| e. Sustained 200 s with `SO_DEVMEM_DONTNEED` recycling | **PASS**, 655 MiB, 484668 tokens returned, 0 mismatches |
| f. Control: same RX bind on the **stock** kernel | **GTT / system RAM**, exactly as predicted, transfer still correct. Section 7 |

`SCM_DEVMEM_LINEAR` count is **0 in every run above** (`linear_cmsgs=0`): no payload ever
reached the linear area. Emulator `rx_dma_error`, `tx_dma_error`, `rx_desc_fetch_error`,
`tx_desc_fetch_error`, `tx_desc_data_error` are 0 on both servers after every case.

## 2. The gating check: the emulator writing into the GPU BAR

Devmem RX makes the NIC DMA *into* the GPU BAR (`DMA_FROM_DEVICE`), where every earlier GPU
result was a read. Two things had to hold, and both do:

- **The server's mapping is writable.** Both NIC servers map the GPU's VRAM memfd
  `rw-s` (`E/00-gating-server-maps.txt`):

      74ad040c0000-74ed040c0000 rw-s 00000000 00:01 367793   /memfd:rocjitsu-vram  (ernic-1)
      780bf00c0000-784bf00c0000 rw-s 00000000 00:01 322249   /memfd:rocjitsu-vram  (ernic-2)

- **The code path asks for the right protection.**
  [`src/ernic_dma.c:82`](../../../src/ernic_dma.c)
  (read only, not modified): `dma_xfer_piece()` calls
  `vfu_addr_to_sgl(..., write ? PROT_WRITE : PROT_READ)`, and `ernic_dma_write()` passes
  `write = true`. A write into a region libvfio-user holds read-only would fail there with
  `-EIO` and raise `rx_dma_error`. It never did: `rx_dma_error` 0 in every case.

**So there is no blocker.** Nothing in this note is gated.

## 3. Method

- **Receiver**: `O/src/devmem_rx_gpu.c` (built in both guests at `~/e2e`, unchanged from my
  predecessor). Binds a dma-buf for RX (`NETDEV_CMD_BIND_RX`), accepts one connection,
  `recvmsg(MSG_SOCK_DEVMEM)`, and for each `SCM_DEVMEM_DMABUF` cmsg reads the bytes **from
  the dma-buf's own memory** — never from the socket — through the GEM mapping (`-G`), the
  parent's HIP device-to-host copy, or a GPU-kernel copy; then returns the tokens with
  `SO_DEVMEM_DONTNEED`. **The buffer is pre-filled with 0xa5**, so a byte the NIC did not
  write cannot pass (the runs report `mismatch_a5` and `mismatch_zero` separately; both 0).
- **Sender**: `~/gputx/devmem_tx_ext2` (udmabuf, GEM VRAM/GTT, or an inherited
  `hsa_amd_portable_export_dmabuf()` fd from `hip_devmem_src2`).
- **Data**: a position-dependent pattern over the sender's buffer with a **random seed per
  run** (`-P $RANDOM...`), so no two runs carry the same bytes and stale buffer contents
  cannot pass. The receiver regenerates the expected stream from the plan.
- **Probe**: `O/scripts/rx-probe.bt` in **both** guests (kprobes on the attach, pin, map,
  `ionic_tx_map_frag`, `mp_dmabuf_devmem_alloc_netmems`, `ionic_rx_fill`, and
  `dma_sync_single`), classifying every address against GPU BAR0 (`0x40_0000_0000`+) and
  System RAM.
- **Runner**: `O/scripts/case.sh LABEL PORT TXSRC RXSRC SIZE SENDS`, plus `sizes.sh` and
  `bench.sh`. `HOLD=1` makes the receiver hold its last batch's tokens after EOF while the
  host hashes the receiving server's VRAM; `O/scripts/plan_bytes.py` computes the expected
  bytes on the host (checked against `gpu/pattern_sha256.py`).
- Sender r102-1, receiver r102-2; receiver at 1 combined channel with `tcp-data-split on`.

## 4. The matrix

All with the `rxfrag` module set on `7.2.6-devmemp2p` unless said otherwise.

| Case | Bytes verified | Mismatches | Linear cmsgs | RX attach / `peer2peer` / pin mask / map path | RX net_iov DMA | RX payload descriptors | TX frag addresses | Host hash of the **receiving** server's VRAM | Evidence |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **a** `A2` udmabuf -> GEM VRAM | 67108864 | 0 | 0 | dynamic `allow_peer2peer=1` / true / **0x6** / `amdgpu_vram_mgr_alloc_sgt` | **38073 of 38073 in BAR0** | **45788 in BAR0**, 64 System RAM (pre-bind fill) | 17611 System RAM | **equal**, 3 of 3 samples | `E/A2-*` |
| **b** `B1` GEM VRAM -> GEM VRAM | 67108864 | 0 | 0 | same | **38212 of 38212 in BAR0** | **46432 in BAR0** | **17633 of 17633 in BAR0** | **equal**, 3 of 3 | `E/B1-*` |
| **c** `C2` hipMalloc -> hipMalloc, HIP D2H readback | 33554432 | 0 | 0 | same | all in BAR0 | 23455 in BAR0 | all in BAR0 | **equal**, 3 of 3 | `E/C2-*` |
| **c'** `C4` hipMalloc -> hipMalloc, **GPU-kernel** readback | 8388608 | 0 | 0 | same | 1024 in BAR0 | 1368 in BAR0 | in BAR0 | not taken | `E/C4-*` |
| **d** `D-vram2vram-1020m` GEM 1020 MiB both ends | 33554432 | 0 | 0 | same, **9 `dma_map_resource` chunks** (512+256+128+64+32+16+8+2+2 MiB) | 24214 in BAR0 | 23177 in BAR0 | in BAR0 | not taken | `E/D-vram2vram-1020m*` |
| **d** `D-hip2hip-960m` hipMalloc 960 MiB both ends | 33554432 | 0 | 0 | same | 24219 in BAR0 | 23249 in BAR0 | in BAR0 | not taken | `E/D-hip2hip-960m*` |
| **e** `E1` GEM VRAM -> GEM VRAM, 200 s | **686817280** | 0 | 0 | same | 357811 in BAR0 | **475088 in BAR0** | in BAR0 | not taken | `E/E1-*` |
| **f** `F1` udmabuf -> GEM VRAM, **stock kernel** | 33554432 | 0 | 0 | **static (`importer_ops` NULL) / false / 0x2 / `drm_prime_pages_to_sg`** | **19103 System RAM** | **23330 System RAM** | n/a | n/a | `E/F1-*` |

`pci_p2pdma_distance_many()` returned **4** in every case, stock kernel included.
`amdgpu_gem_info` while bound reads `VRAM VISIBLE pin count 1` on both guests for every
patched-kernel GEM case, and `GTT pin count 1` for the stock control.

### The host-side proof (b)

With the receiver holding its last batch's tokens, the largest held fragment was
stream position 67053616, dma-buf offset 49590272, 1448 bytes; the dma-buf's single VRAM
mapping was `dma=0x4038000000`, so that fragment sits at BAR0 offset `0x3af4b000`. The
**receiving** server's `/memfd:rocjitsu-vram` at that offset hashes

    cda2d358...

in 3 of 3 samples, **equal** to the sha256 the host computed for those bytes of the sent
stream; a control region elsewhere in the same BAR differs (`E/B1-vram-to-vram.txt`).
Cases `A2` and `C2` give the same result with their own random seeds.

### Sizes (d)

VRAM GEM both ends, 32 MiB transferred per run, 2 reps each (`E/D-sweep-summary.txt`):

| Buffer | r1 | r2 |
| --- | --- | --- |
| 1 MiB | PASS but 3.3 MiB/s | **FAIL**, finding F2 |
| 4 MiB | PASS 7.2 MiB/s | PASS 5.2 |
| 16 MiB | PASS 7.3 | PASS 3.2 |
| 64 MiB | PASS 3.7 | PASS 8.6 |
| 256 MiB | PASS 4.5 | PASS 10.2 |
| 1020 MiB | PASS (single run) | |

`hipMalloc` both ends: 16, 64, 256 and 960 MiB all PASS. `hipMalloc` above 960 MiB was not
retried (the earlier note found HIP's own limit between 960 and 1016 MiB).

### Sustained (e)

200.8 s, 655 MiB, 655 sends, 484668 `SCM_DEVMEM_DMABUF` cmsgs all returned with
`SO_DEVMEM_DONTNEED`, 118 verify batches, 0 mismatches, 0 linear, every RX payload
descriptor in BAR0, `rx_unreadable_drop` +24 over the run (the idle-queue drops of
finding F1 in `rocm102-rx.md`), 0 dma-buf objects and 0 dmabuf page pools afterwards
(`E/E1-sustained-vram.txt`).

## 5. What the probes say about the things to look for

| Question | Answer, from the runs |
| --- | --- |
| Header split leaving payload in the linear area | **Never.** `linear_cmsgs=0` in every devmem RX run, including 1448-byte maximum fragments and the 200 s run. `rx_hds_split` rises by the frame count |
| Page pool / net_iov chunking over a BAR region | Works, including a buffer the exporter maps as **9 separate `dma_map_resource` chunks** (1020 MiB). Every net_iov DMA address is 4 KiB-aligned (`rx_niov_dma_low12[0]` = all) and inside BAR0 |
| amdgpu moving or evicting the buffer while bound | **No.** `pin count 1` for the whole binding, KFD `evicted_ms` 0 on both guests in every patched-kernel case; HIP's own read of the buffer after the bind is correct (the stock-kernel eviction defect F1 of `rocm102-gpu.md` does not arise, because the buffer never leaves VRAM) |
| ZONE_DEVICE pages vs net_iov in the RX path | The devmem provider hands the pool **net_iovs** (`mp_dmabuf_devmem_alloc_netmems`), never struct pages; the driver posts their DMA addresses straight into the SG element of the RX descriptor. The header buffer in the same descriptor stays in System RAM — header split is what makes this work |
| The emulator writing into a region mapped read-only | Does not happen: section 2 |
| DMA sync on a P2P address | **Never seen.** `@dma_sync_on_gpu_bar0` is absent from every probe output: no `dma_sync_single_for_cpu/device` was called with a BAR0 address |

## 6. Findings

- **F1. The GPU-kernel readback path hangs at scale** (`E/C3-hip-to-hipkernel.txt`).
  `hip_devmem_rx -c` (a GPU kernel copies the NIC-written VRAM buffer into a second
  `hipMalloc` allocation, which is then read device-to-host) verified 25577320 bytes with 0
  mismatches over 17 batches and then stopped making progress. The main thread spins at 91 %
  CPU in userspace (`State: R`), with **every amdgpu ring fence signalled**
  (`amdgpu_fence_info`: last emitted == last signalled on all rings) and KFD `evicted_ms` 0 —
  a HSA signal the runtime waits on that never completes, the same shape as F1 in
  `rocm102-gpu.md`. **It is not devmem**: the devmem RX side had delivered and verified every
  byte up to that point, the `hipMemcpy` readback of the same workload (`C2`) passed 23
  readbacks, and the smaller GPU-kernel run (`C4`, 8 MiB, 22 readbacks) passed. The GPU is not
  wedged: after killing the process a fresh HIP process ran normally.
  **Reproducer**: `O/scripts/case.sh C3 5426 hip-kernel hip-kernel 16m 32`, with
  `~/e2e/hip_devmem_rx -c`. Not diagnosed further (a product fix is out of scope).
- **F2. A devmem RX dma-buf of 1 MiB starves the page pool and collapses the connection**
  (`E/D-1m-stall-diagnosis.txt`, `E/D-vram2vram-1m-r2.txt`). With `rings rx 1024` the driver
  wants far more buffers than a 256-page dma-buf can provide:
  `mp_dmabuf_devmem_alloc_netmems` failed **9995** times in 100 s while only 256 net_iovs
  existed. The NIC then has nothing to post, frames are dropped, and TCP falls into RTO
  backoff (`ss -ti`: `cwnd:1 backoff:7 rto:39424 lost:207 rwnd_limited:99.7%`). The receiver
  ended `end=idle-timeout` after 26406984 of 33554432 bytes — **with 0 mismatches**: nothing
  was corrupted, the stream merely stopped. One of the two 1 MiB runs did finish, slowly.
  4 MiB (1024 pages = the ring size) is the smallest size that worked in both reps, and even
  there the pool momentarily ran dry (1883 failures). **Not a regression** — it is the
  pool-size-versus-ring-size relation, and it is the same with udmabuf; it is worth a
  documented minimum, or refusing a bind whose dma-buf cannot fill the ring.
- **F3. Harness lesson:** `-H` (tokens held before a verify batch) must stay well below the
  dma-buf's page count, or the receiver itself starves the pool. `-H 20000` on a 4096-page
  buffer reproduced F2 deterministically (`E/C1-hip-to-hip.txt`, rerun as `C2`).
- **F4.** Reading back a VRAM buffer through its GEM CPU mapping costs about 3 MiB/s on this
  emulated GPU, which is what limits every "VRAM -> VRAM" rate below. It is the *verifier*,
  not the datapath.

## 7. The control: the same bind without the backport (f)

r102-2 only was moved to the stock `7.2.6-070206-generic` (`kernel-select2.sh stock 2`, cold
cycle), the `rxfrag` set rebuilt for it in the guest (`ionic-mod.sh build rxfrag 2`, srcversion
`0FDEA047AB0132E8F7AB9C8`, vermagic `7.2.6-070206-generic`) and loaded; r102-1 stayed on
`7.2.6-devmemp2p`. `quick-start test` base passed (rc 0) in that mixed state.

**The RX bind of a VRAM-created GEM BO succeeds and the transfer is correct, but the buffer
is not in VRAM** (`E/F1-control-stock-vramrx.txt`):

| | stock `7.2.6-070206-generic` | `7.2.6-devmemp2p` |
| --- | --- | --- |
| `dma_buf_attach` kind | **static (`importer_ops` NULL)** | dynamic, `allow_peer2peer=1` |
| `attach->peer2peer` after amdgpu's attach | **false** | true |
| Pin domain mask | **0x2 (GTT only)** | 0x6 |
| Map path | **`drm_prime_pages_to_sg`** | `amdgpu_vram_mgr_alloc_sgt` |
| `amdgpu_gem_info` while bound | **`GTT pin count 1`** | `VRAM VISIBLE pin count 1` |
| net_iov DMA addresses | **19103 System RAM, 0 BAR0** | all BAR0 |
| RX payload descriptors | **23330 System RAM, 0 BAR0** | all BAR0 |
| `pci_p2pdma_distance_many()` | 4 | 4 |
| Transfer | PASS, 33554432 bytes, 0 mismatches | PASS |

**Why**: the same line as for TX. `net/core/devmem.c` on v7.2.6 calls plain
`dma_buf_attach()`, so `attach->peer2peer` stays false
(`drivers/dma-buf/dma-buf.c:1048`), and amdgpu's `amdgpu_dma_buf_pin()` strips
`AMDGPU_GEM_DOMAIN_VRAM` from the pin domains for any attachment that is not peer-to-peer
(`amdgpu_dma_buf.c:357`). Commit **69ab87acc614** ("net: devmem: allow peer-to-peer dma-buf
attach for bindings", v7.4-rc1), backported here, is the whole difference — **the one commit
covers both directions**: no RX-specific core change was needed.

r102-2 was then put back on `7.2.6-devmemp2p` (section 9).

## 8. Rates (emulation numbers on a loaded host; 3 samples each, 64 MiB per run)

Single samples on this host vary by more than 2x, so these bound the order of magnitude and
nothing more. No rate difference below is claimed from one sample.
(`E/BENCH-summary.txt`, `E/BM-*`.)

| Kind | tx MiB/s (3 samples) | rx MiB/s |
| --- | --- | --- |
| copy TCP (`-m copy`) | 191.5, 226.4, 206.2 | 183.2, 215.7, 196.8 |
| `MSG_ZEROCOPY` (`-m zc`) | 240.0, 215.9, 177.1 | 229.5, 202.0, 169.9 |
| devmem TX (udmabuf) -> plain RX | 184.1, 218.3, 195.7 | 178.5, 208.6, 186.4 |
| devmem TX -> **devmem RX** (udmabuf both) | 145.4, 94.0, 103.4 | 136.3, 86.6, 96.2 |
| **VRAM -> VRAM** (GEM both) | 4.3, 4.4, 4.4 | 3.2, 3.3, 3.3 |

The VRAM row is dominated by the two CPU mappings of emulated VRAM: the sender fills its BO
through one and the receiver verifies through the other (F4). It is not a measurement of the
devmem datapath.

## 9. State left

**Stack 3 is up and healthy, both guests on `7.2.6-devmemp2p` with the `rxfrag` set loaded**
(see section 10 for the exact checks).

| Item | State |
| --- | --- |
| Kernels | r102-1 and r102-2 both `7.2.6-devmemp2p`; `zz-devmem.cfg` present in both |
| Modules | `rxfrag`: ionic srcversion `0FDEA047AB0132E8F7AB9C8`, `ionic_rdma` `24E57766CB83B79E273C7B0`, `ernic_p4` `66D2E2195859E80AE848D07`, by `insmod` from `~/ionic-mod/rxfrag` (not persistent); amdgpu and `sdxi_char` loaded |
| Servers | unchanged: `B/ernic/poc-rel` (2d3a3d02, Release) through `verify/hs-wrapper.sh`, `compat + header-split`, no P4 program |
| Driver settings | restored to the defaults I found: MTU 1500, 4 combined channels, rings 1024, `tcp-data-split off` |
| Addresses | 10.102.100.11 / .12, IPv4 GID at index 1 |
| Added to guest disks | `~/e2e/` was already there; r102-2's `~/ionic-mod/rxfrag` now also holds a build for the stock kernel's vermagic, overwritten by the devmemp2p rebuild |
| Host | `O/scripts/` (new: `case.sh`, `sizes.sh`, `bench.sh`, `plan_bytes.py`), `E/` (79 files) |
| Stacks 1 and 2 | untouched (both up 15 hours) |
| Last checks (`E/99-final-state.txt`) | `quick-start test` base **rc 0** on this boot; both guests `7.2.6-devmemp2p`, ionic `0FDEA047AB0132E8F7AB9C8`, amdgpu + `sdxi_char` loaded, MTU 1500, 4 channels, rings 1024/1024, `tcp-data-split off`, 0 dma-buf objects, 0 dmabuf page pools, **0 BUG/WARNING/Oops/Call Trace lines**; all five emulator error counters 0 on both servers |

## 10. What real hardware would still have to show

Everything here is an emulated NIC and an emulated GPU in one host, with no IOMMU, so a DMA
address is a guest physical address and "the NIC wrote into the GPU's BAR" is a memfd write
by one container. On real hardware the following are still open:

- **A real PCIe P2P write** from the NIC into the GPU's BAR, through a real root complex,
  with `pci_p2pdma_distance()` deciding on the real topology rather than on the
  `AuthenticAMD`/family-23 path this guest takes. ACS, the root port's P2P support, and
  whether traffic is routed peer-to-peer or bounced through host memory are all untested.
- **An IOMMU in the path**: every address here is a GPA. With an IOMMU the dma-buf map returns
  IOVAs and `dma_map_resource()` has to program the IOMMU for a BAR window.
- **Write ordering and visibility**: whether the GPU sees the NIC's writes without an explicit
  fence or sync. No `dma_sync_single_*` was called on a BAR0 address here (section 5); on real
  hardware that may be where a missing barrier shows.
- **Rates**: nothing in section 8 is a hardware number; the emulated VRAM mapping alone costs
  two orders of magnitude.
- **The GPU-kernel hang (F1)** against a real KFD and a real queue manager.
- **Large MTU / multi-SG payloads into VRAM**: the stack's L2 is 1500, so a payload never
  needed more than one SG element per frame (`rocm102-rx.md` 4.4). **NOT RUN.**
- **NOT RUN here either**: devmem RX into a GPU buffer with an IPv6 or VLAN flow; two bound
  queues; `rmmod` with a live VRAM binding; a second non-peer-to-peer attachment to a pinned
  buffer (the limit 69ab87acc614's commit message names); GPU writes into the buffer while the
  NIC has it bound for RX; a sub-allocation exported at a non-zero offset (`-O`); the stock
  kernel with a `hipMalloc` RX buffer (only the GEM control was run).

## 11. Files

    O/src/      devmem_rx_gpu.c  hip_devmem_rx.cpp        (my predecessor's, unchanged)
    O/scripts/  env.sh install.sh rx-probe.bt             (unchanged)
                case.sh sizes.sh bench.sh plan_bytes.py   (new)
    E/00-gating-server-maps.txt     the gating check
    E/A*, E/B*, E/C*                cases a, b, c (and C1/C3, the two that failed)
    E/D-*                           sizes, D-1m-stall-diagnosis.txt, D-sweep-summary.txt
    E/E1-*                          the 200 s sustained run
    E/F1-*                          the stock-kernel control
    E/BENCH-summary.txt, E/BM-*     the rates
    E/99-final-state.txt            the state the stack was left in
    Every case file has -rxprobe.txt and -txprobe.txt beside it.
