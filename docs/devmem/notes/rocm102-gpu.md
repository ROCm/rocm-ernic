# Stack 3 (`ionic-devmem-r102`): GPU buffers as the devmem TCP TX source on the ROCm 10.2 guest

2026-11-20, 01:19Z to 02:13Z (evidence is stamped in UTC). Everything here was RUN unless marked
**NOT RUN**. `B` = `/var/tmp/ionic-devmem/test-rocm102`, `E` = `B/evidence/gpu`, `Q` =
`/var/tmp/ionic-devmem/qemu-minimal-2.1` (v2.1.0). Stack as described in
[rocm102-baseline.md](rocm102-baseline.md) and [rocm102-devmem.md](rocm102-devmem.md): server
2d3a3d02 (Release), ionic = srq 0001-0011 + p4 + devmem-tx, amdgpu-dkms 31.70 (module 7.2.3),
ROCm 10.2, sender r102-1, receiver r102-2. Nothing in rocm-ernic, qemu-minimal or stacks 1 and 2
was changed.

## Answer

| Question | Result |
| --- | --- |
| 1. Devmem TX from an amdgpu GEM buffer on the stock 7.2.6 guest | **Works, payload verified; placement is GTT / system RAM**, as expected |
| 2. Devmem TX from a `hipMalloc` buffer exported with `hsa_amd_portable_export_dmabuf()` | **Works, payload verified in 5 of 5 runs; placement GTT / system RAM.** The bind migrates the compute buffer out of VRAM, and HIP's own next read of it then copied nothing (3 runs) or hung (1 run): a GPU-stack problem, section 3 |
| 3. Is the stack P2P-capable? | **Yes.** Upstream's ROCm RDMA dma-buf test on the same boot: dynamic peer-to-peer attach, buffer pinned in VRAM, all 33 MR page-table entries inside GPU BAR0, the receiver's NIC server seen writing into its `rocjitsu-vram` mapping |
| 4. What it takes to send from VRAM | **One 47-line core commit.** Backported to v7.2.6, built, booted in r102-1: GEM and `hipMalloc` buffers both stay in VRAM, every descriptor address is in GPU BAR0, the server's VRAM mapping holds the pattern, payload verified, HIP unaffected |

## 1. The address map and the probe

Both guests (`E/00-guest-facts-r102-N.txt`):

| Range | What |
| --- | --- |
| `0x40_0000_0000`–`0x7f_ffff_ffff` | GPU `02:00.0` BAR0, 256 GiB; amdgpu registered `0x4000000000`–`0x403fffffff` (1024 MiB, `vramlimit`) as peer-to-peer memory |
| `0x1000`–`0x7ffd_bfff`, `0x1_0000_0000`–`0x2_7fff_ffff` | System RAM |
| `0xfe00_0000`, `0xfe40_0000` | NIC `01:00.0` BAR2 / BAR0 |

- **Kernel:** `CONFIG_PCI_P2PDMA=y`, `ZONE_DEVICE=y`, `HSA_AMD_P2P=y`, `NET_DEVMEM=y` (confirmed).
  No IOMMU, so a DMA address is a guest physical address.
- **P2P distance:** NIC and GPU sit under different root ports (`00:02.0`, `00:03.0`);
  `pci_p2pdma_distance_many()` returns **4** in every case. As on stack 1 this relies on the
  guest CPU being `AuthenticAMD` family 23 (host bridge `8086:29c0` is not whitelisted).
- **Probe:** `B/scripts/gpu/gpu-probe.bt` (kprobes, vmlinux types; the DKMS modules have no BTF).
  It records the attach kind in `dma_buf_dynamic_attach()`, `attach->peer2peer` on return from
  `amdgpu_dma_buf_attach()`, the domain mask `amdgpu_dma_buf_pin()` hands `amdgpu_bo_pin()`
  (GTT 0x2, VRAM 0x4), which map path ran, the address `dma_map_resource()` returned for a VRAM
  map, every `ionic_tx_map_frag()` return classified against the table above, and for RDMA the
  MR page table `ionic_pgtbl_init()` built.
- **Runner:** `B/scripts/gpu/run-gpu-case.sh LABEL PORT SECS udmabuf|gem-vram|gem-gtt|hip|hip-kernel`.
  The receiver regenerates the stream from the plan and compares every byte. Placement
  (`amdgpu_gem_info`, `amdgpu_vm_info`, `dma_buf/bufinfo`, KFD `evicted_ms`) is captured while
  the binding is live.

## 2. Stock kernel `7.2.6-070206-generic`: GEM buffer (question 1)

Tool: `~/gputx/devmem_tx_ext`, built in the guest from `B/src/gpu/devmem_tx_ext.c`
(`devmem_tx_gpu.c` plus `-E FD -O OFF` for an inherited dma-buf; `-G`/`-J` unchanged).

| Case | Source | Bytes verified at r102-2 | Attach | `peer2peer` | Pin mask | Map path | `amdgpu_gem_info` while bound | Frag addresses | `tx_dma_error` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `G0` | udmabuf (control) | 846525395 | static | n/a | n/a | n/a | n/a | 222747 System RAM | 0 |
| `G1` | GEM, created VRAM (`-G`) | 1162734881 | static | false | 0x2 | `drm_prime_pages_to_sg` | `GTT pin count 1 ... VRAM_CLEARED` | 305915 System RAM, 0 BAR0 | 0 |
| `G2` | GEM, created GTT (`-G -J`) | 1294438832 | static | false | 0x2 | `drm_prime_pages_to_sg` | `GTT pin count 1` | 340607 System RAM | 0 |

0 mismatches, `copied=0`, completions = sends in each. **Confirmed: works, from system RAM.**

**The line that decides it in this guest's kernel:** `net/core/devmem.c:229`,

    binding->attachment = dma_buf_attach(binding->dmabuf, dma_dev);

- `drivers/dma-buf/dma-buf.c:1081-1085`: `dma_buf_attach()` is
  `dma_buf_dynamic_attach(dmabuf, dev, NULL, NULL)`.
- `dma-buf.c:1048-1049`: `attach->peer2peer` is set only `if (importer_ops)`, so it stays false.
- `dma-buf.c:1201-1202`: a static attachment is pinned at map time (`dma_buf_pin_on_map()`).
- Guest `/usr/src/amdgpu-7.2.3-2428518.26.04/amd/amdgpu/amdgpu_dma_buf.c:357-358`: any
  attachment with `!attach->peer2peer` removes `AMDGPU_GEM_DOMAIN_VRAM` from the pin domains;
  `:366` pins what is left (GTT).

The P2P gate itself (`amdgpu_dma_buf.c:278`, `pci_p2pdma_distance()`) passes with 4; nothing asks.

## 3. Stock kernel: `hipMalloc` buffer (question 2)

**The export works.** `B/src/gpu/hip_devmem_src.cpp` (built in the guest with `hipcc`): `hipMalloc`
4 MiB, fill with the test pattern (`hipMemcpy` host to device, or `-k` a GPU kernel), read back
and compare, `hsa_amd_portable_export_dmabuf()`, then fork and exec the sender with the fd while
the HIP process stays alive. That is the call perftest makes: the guest's `ib_write_bw` imports
`hsa_amd_portable_export_dmabuf@ROCR_1` (`nm -D`). Export returned fd, offset 0, dma-buf size
4194304 every time.

| Case | Fill | Bytes verified at r102-2 | Placement while bound (`amdgpu_vm_info`) | Frag addresses | KFD `evicted_ms` | HIP read of the buffer afterwards |
| --- | --- | --- | --- | --- | --- | --- |
| `G3-hip-first` | `hipMemcpy` | 1769153073 | not captured | 465448 System RAM | not captured | differs |
| `hip-hang/G3-hip-HUNG` | `hipMemcpy` | 1273138193 | not captured | 334966 System RAM | 102 | **hung** (killed after 2 min) |
| `G3` | `hipMemcpy` | 1628148843 | `GTT pin count 1 exported` | 428366 System RAM | 103 | copies nothing |
| `G4` | GPU kernel | 1399441982 | `GTT pin count 1 exported` | 368196 System RAM | 105 | equals the pattern |
| `G5` | `hipMemcpy` | 1681650448 | `GTT pin count 1 exported` | 442422 System RAM | 107 | copies nothing |

Static attach, `peer2peer` false, pin mask 0x2, `drm_prime_pages_to_sg`, `tx_dma_error` 0 in all.
**The NIC side is correct in all five: 0 mismatches.**

**Finding F1: after the bind, HIP's own access to the buffer misbehaves, and the cause is not
devmem.**

- **Symptom:** `hipMemcpy` device to host returns success and leaves the destination untouched
  (4177995 of 4194304 bytes still the 0xa5 pre-fill, three attempts over 5 s), or the process
  spins in `rocr::core::BusyWaitSignal::WaitRelaxed` for good. A host-to-device write followed by
  a read works again.
- **Trigger:** the pin moves the buffer VRAM to GTT, which evicts and restores the KFD process
  (`evicted_ms` 102 to 107).
- **Attribution, three controls with no NIC involved** (`E/G3a`, `G3b`, `G3c`):
  - export only, child `/bin/true`: reads back fine;
  - fork with a child that sleeps 2 s, `evicted_ms` 0: reads back fine;
  - child reads `/sys/kernel/debug/dri/1/amdgpu_evict_vram`, `evicted_ms` 169: **same failure**.
- **So:** a KFD evict/restore cycle on this emulated GPU breaks the process's next device-to-host
  copy. Whether the defect is in amdgpu-dkms 31.70's KFD or in rocjitsu was not determined.
- **Consequence on stock 7.2.6:** binding HIP memory for devmem TX always forces that cycle.
  With the core change (section 5) the buffer never moves: `evicted_ms` 0 and HIP reads back
  correctly in 4 of 4 runs.

**NOT RUN:** GPU writing into the buffer while the NIC has it bound; buffers above 4 MiB;
`hsa_amd_portable_export_dmabuf` of a sub-allocation (non-zero offset; `-O` exists for it).

## 4. The contrast: upstream's ROCm RDMA dma-buf test (question 3)

`B/scripts/gpu/rdma-dmabuf-probed.sh` runs `quick-start test --rocm-rdma-test --rocm-rdma-mode
dmabuf` with the probe in both guests (`E/R1`, `E/R2`: pass, 1237 and 1301 MiB/s, `VALIDATION:
PASSED - 7 chunks, 58720256 bytes`). `rdma-dmabuf-live.sh` runs the same command lines by hand
with `-n 100000` and samples the servers' memory from the host (`E/R3`: 1781 MiB/s, validated).

| Evidence | RDMA dma-buf MR (both guests) | Devmem TX, stock kernel |
| --- | --- | --- |
| Attach (`dma_buf_dynamic_attach`) | **dynamic, `allow_peer2peer=1`** (`ib_write_bw`) | static |
| `pci_p2pdma_distance_many()` | 4 | 4 |
| `attach->peer2peer` after amdgpu's attach | **true** | false |
| Pin domain mask | **0x6** (VRAM allowed) | 0x2 |
| Map path | **`amdgpu_vram_mgr_alloc_sgt`**, `dma_map_resource` returned `0x4038000000` (64 MiB) and `0x403fa00000` (2 MiB) | `drm_prime_pages_to_sg` |
| Addresses given to the device | **33 of 33 MR page-table entries in GPU BAR0** (2 MiB pages, `0x4038000000`...) | all in System RAM |
| Emulator | `rx/tx_dma_error` 0, remote and local access errors 0, 0 `could not gather` lines | `tx_dma_error` 0 |

**Do the emulator's accesses resolve into `rocjitsu-vram`? Yes, seen from the host** (`E/R3-*`):
each NIC server maps a 262144 MiB `/memfd:rocjitsu-vram`; hashing its 64 MiB at BAR0 offset
`0x38000000` every 0.15 s through `/proc/1/fd` in the container shows the **receiver's** region
all zero before the run, then perftest's initial fill, then 67108808 non-zero bytes with 28
distinct hashes over 30 samples during the transfer, and zero again after perftest freed the buffer. Only the NIC server and the
GPU emulator write that memory, and the address is the one in the MR's page table.

- **Not claimed:** byte equality of sender and receiver regions. The one hash seen on both
  servers is perftest's identical initial fill, not the transfer; correctness of the data rests
  on perftest's own validation.
- **Upstream's own proof** of VRAM placement is the stock-QEMU versus fork-QEMU contrast (access
  error against pass) plus the server's `maps` (`Q/AGENTS.md`, "perftest with ROCm memory
  support"). It has no address probe; the table above adds one.

## 5. What it takes to send from VRAM (question 4)

### 5.1 The upstream change

`git log -S` on `net/core/devmem.c` in `/var/tmp/ionic-devmem/linux` for
`dma_buf_dynamic_attach`, `allow_peer2peer` and `dma_buf_pin` all give **one commit**:

    69ab87acc614  net: devmem: allow peer-to-peer dma-buf attach for bindings
    Erfan Sharafzadeh, 2026-10-13; first in v7.4-rc1; net/core/devmem.c, +37 -10

It switches to `dma_buf_dynamic_attach()` with `.allow_peer2peer = true`, pins with
`dma_buf_pin()` before mapping, waits for the exporter's kernel fences, and unpins on release.

### 5.2 Option (a): carry it on v7.2.6. Done here as an experiment, and it works

- **Backport:** the upstream patch does not apply as is (3 of 6 hunks fail on context: v7.2.6
  still has the genpool allocator). The hand backport is 100 lines of diff:
  `B/kbuild/0001-net-devmem-allow-peer-to-peer-dma-buf-attach-v7.2.6-backport.diff` (copy in
  `E/core-patch/`, with the upstream patch and the failed dry run). It needs nothing else from
  the dma-buf core.
- **Kernel:** `7.2.6-devmemp2p`, built in `devmem-kbuild:1` from the guest's own
  `/boot/config-7.2.6-070206-generic` reduced with `localmodconfig` (RUST, module signing-all and
  `BUILTIN_MODULE_RANGES` off), installed beside the stock kernel **in r102-1 only**. DKMS rebuilt
  amdgpu 31.70 for it unchanged; the devmem ionic was rebuilt with `ionic-mod.sh build devmem 1`.
  Scripts: `B/scripts/gpu/kbuild.sh`, `kernel-select.sh p2p|stock`.

| Case on `7.2.6-devmemp2p` | Bytes verified at r102-2 | Attach / `peer2peer` / pin mask | Placement while bound | Frag addresses | Other |
| --- | --- | --- | --- | --- | --- |
| `P0` udmabuf | 1167735031 | dynamic p2p / n/a / n/a | n/a | 307231 System RAM | udmabuf still works |
| `P1` GEM VRAM | 1521045630 | dynamic p2p / true / 0x6 | **`VRAM VISIBLE pin count 1`** | **400196 of 400196 in GPU BAR0** | `dma_map_resource` = `0x403f800000` |
| `P2` GEM GTT (`-J`) | 1434243026 | dynamic p2p / true / 0x2 | `GTT pin count 1` | 377380 System RAM | the control moves the bucket |
| `P3` GEM VRAM | 2462673878 | as `P1` | VRAM | 647894 in BAR0 | **server memory check, below** |
| `P4` `hipMalloc` | 1987759631 | dynamic p2p / true / 0x6 | VRAM | **522947 of 522947 in BAR0** | **server memory check; HIP read back equal** |
| `P5` `hipMalloc`, GPU-kernel fill | 1223936717 | same | `VRAM VISIBLE pin count 1 exported` | 322004 in BAR0 | `evicted_ms` 0; HIP equal |
| `P6` `hipMalloc` | 1140434212 | same | same | 300038 in BAR0 | `evicted_ms` 0; HIP equal |
| `P7` `hipMalloc`, 20 s | 4402532072 | same | same | 1158221 in BAR0 | `evicted_ms` 0; HIP equal |
| `P8` RDMA dma-buf, by hand | validated, 1393 MiB/s | dynamic p2p | VRAM | 33 of 33 MR entries in BAR0 | no regression |

0 mismatches, `copied=0`, `tx_dma_error` 0 everywhere; no kernel log splat; 0 dma-buf objects and
VRAM usage back to its idle value afterwards (`E/P9`).

**Server memory check** (`B/scripts/gpu/vram-memfd-check.sh`, `E/P3`, `E/P4`): while the binding
is live, the 4 MiB at BAR0 offset (descriptor address minus `0x4000000000`) of the **sender's NIC
server's** `/memfd:rocjitsu-vram` has sha256 `b78daf2d...`, equal to the pattern's hash computed
on the host, in 3 of 3 samples in both cases; the 4 MiB below it differ; after the sender exits
the region hashes as zeros. The bytes the NIC was told to send are in the GPU's memory.

**What option (a) costs as a product change:**

- **Exists:** the backport (built, run), the build recipe, the proof.
- **Missing:** the image uses the unpatched mainline-PPA kernel, so this means a custom kernel
  package in the bake: build or host the deb with the PPA's full config (only a `localmodconfig`
  build was made here; full-config build **NOT RUN**), move the pinned release string
  (`GRUB_TOP_LEVEL`, the checks in `Q/qemu/checks/rocm-golden.sh`), and build for it the modules
  that do not autoinstall: `ionic-ernic` and `ernic-p4` (`AUTOINSTALL="no"`) and `sdxi_char`
  (`updates/`, not DKMS). amdgpu autoinstalls.
- **Estimate:** 2 to 4 days in qemu-minimal's bake and CI, plus carrying one core patch until the
  guest kernel reaches v7.4. **NOT RUN on the patched kernel:** `quick-start test` and its
  options, the devmem matrix, SDXI, hipFile, a second non-P2P attachment to a pinned buffer (the
  limit the upstream commit message names).

### 5.3 Option (b): move the image to a v7.4 kernel

| Piece | State |
| --- | --- |
| Core change | in v7.4-rc1 and later, nothing to carry |
| amdgpu-dkms 31.70 on v7.4 | **exists:** `/var/tmp/ionic-devmem/amdgpu-31.70/patches/0001`-`0005`, built and run on v7.4-rc3 with devmem TX from VRAM verified (`amdgpu-31.70/README.md`). Drop 0006 and 0007 there: upstream's `ras_vbios` and `amd_vsdb_freesync` in `amdgpu-emu-patches.py` cover the same two bugs |
| ionic SRQ series | **mostly gone:** srq 0001-0008 are upstream in v7.4-rc3 (`git log v7.2..v7.4-rc3`). srq 0009-0011 and p4 0001 apply to v7.4-rc3 as they are (`E/v74-series-check/`; apply check only, **not built**) |
| ionic devmem TX | the six-patch RFC is written against v7.4-rc3 and was run there on stack 1; it applies on top of the four above |
| ROCm 10.2 userspace on v7.4 | **unknown:** stack 1's v7.4 run used a raw GEM buffer; no HIP workload, KFD test, perftest, hipFile or SDXI has run on v7.4. **NOT RUN** |
| Kernel | v7.4 is at rc3; the image would pin an rc build |

**Estimate:** 1 to 2 weeks, most of it re-running every `quick-start test` option on v7.4 and
moving `IONIC_KERNEL_REF` and `ernic_source_pin`. It is the durable answer: no carried core
patch, and the SRQ series shrinks from eleven patches to three.

**Recommendation:** (a) is the short path and is proven on this guest today; (b) is where the
image should end up. On the stock image, devmem TX from GPU buffers works but reads system RAM
and, for HIP memory, runs into F1.

## 6. Other findings

- **F2. Reading `/sys/kernel/debug/kfd/hqds` oopses this guest** (NULL dereference in
  `hqd_sdma_dump_v12_1`, amdgpu 31.70 on gfx1250, twice, `E/hip-hang/dmesg-r102-1.txt`). It was
  my diagnostic, after the hang, not its cause. The reader dies with interrupts disabled, so I
  cold-cycled the stack. Do not read that file.
- **F3. `ionic-mod.sh load` can lose a race with udev** (`insmod ionic_rdma.ko: Device or
  resource busy`, 16 s after a boot, `E/hip-hang/cycle1-load-EBUSY.log`): the refused autoload
  of the baked `ionic_rdma` was still in flight. The script now runs `udevadm settle` first.
- **F4. Harness lessons.** The sender's output is block-buffered, so "wait for the bind line"
  captured placement at exit (`E/G1-gem-vram-capture-raced.txt`); the runner now waits for the
  NIC's attachment in `dma_buf/bufinfo`. `ionic_pgtbl_page()` is inlined; a bpftrace range loop
  in that kretprobe ran no iterations and said nothing (`unroll` works). VRAM is wiped on free,
  so host-side memory checks must sample during the run (`E/R2-memfd-after-run.txt`).
- **F5. Stack 1 was restarted at 02:09Z by another session** working under
  `/var/tmp/ionic-devmem/overnight/`, not by this work. Stack 2 is untouched (up 22 hours).

## 7. State left

- **Stack:** running and healthy, cold-cycled at 02:10Z; both guests on the stock
  `7.2.6-070206-generic`.
- **Servers:** `B/ernic/poc-rel/build/rocm-ernic` (2d3a3d02, Release), as before.
- **Guest modules:** devmem ionic, `ionic_rdma`, `ernic_p4` loaded with `insmod` (same build-ids
  as before); amdgpu loaded; addresses 10.102.100.11 / .12; IPv4 GID at index 1 on both.
- **Added to r102-1's disk:** kernel `7.2.6-devmemp2p` (image and headers packages, amdgpu DKMS
  build for it), **not selected**. `kernel-select.sh p2p; stack.sh cycle; ionic-mod.sh build
  devmem 1; ionic-mod.sh load devmem; sudo amdgpu-probe` returns to it; `kernel-select.sh stock`
  and the same steps come back. `~/ionic-mod/devmem.old-*` hold the earlier builds.
- **Added to both guests:** `~/gputx/` (`devmem_tx_ext`, `hip_devmem_src`, `gpu-probe.bt`, sources).
- **Last checks** (`E/final/`, `B/evidence/quickstart/gpu-final-base/`): base `quick-start test`
  PASS, devmem smoke PASS, SRQ PASS, TCP sha256 both ways PASS, GEM-VRAM devmem on stock back to
  GTT (`E/Z1`), kernel logs clean, 0 dma-buf objects.
- **Host:** container `ionic-devmem-r102-kbuild` (exited) and `B/kbuild/` (source copy, build
  tree, debs; 12 GB) remain.

## 8. Files

    B/src/gpu/devmem_tx_ext.c, hip_devmem_src.cpp
    B/scripts/gpu/  gpu-probe.bt  install.sh  run-gpu-case.sh  rdma-dmabuf-probed.sh
                    rdma-dmabuf-live.sh  vram-memfd-check.sh  pattern_sha256.py
                    kbuild.sh  kbuild-in-container.sh  kernel-select.sh
    B/kbuild/       0001-...-v7.2.6-backport.diff, config, debs
    E/G*            stock kernel devmem cases; E/hip-hang/ the hang and the oops
    E/R*            RDMA dma-buf contrast; E/P* patched-kernel cases
    E/core-patch/   upstream commit, dry run on v7.2.6, backport
    E/v74-series-check/  ionic series against v7.4-rc3
