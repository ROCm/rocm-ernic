# (A) amdgpu dma-buf as the devmem TX source — result

Run 2026-11-19 on the `ionic-devmem` stack (stack 1), guests `devmem-1`
(sender) and `devmem-2` (receiver). Evidence under
`test/evidence/gpu/` (files `A1`–`A6`). Nothing in the emulator was changed.

## Answer

**Yes — a dma-buf exported by amdgpu from the emulated rocjitsu GPU works as
the devmem TX source, end to end, with the payload verified at the peer.**

**No — the NIC does not read VRAM.** The buffer, created with
`AMDGPU_GEM_DOMAIN_VRAM`, is migrated to **GTT / system pages** the moment the
NIC pins it, and every DMA address the ionic driver puts in a TX descriptor
lies in **System RAM**, not in the GPU's BAR. This is the verified expectation
from [ernic-gpu-dmabuf.md](ernic-gpu-dmabuf.md) §3 and it is now observed, not
inferred.

## What it took to get a working GPU (the previous agent's blockers, re-checked)

| Earlier finding | Re-check |
| --- | --- |
| DKMS amdgpu 7.1.9 does not compile against 7.4.0-rc3 headers | **Confirmed**, not retried (`A1`, `dkms-amdgpu-7.1.9-make-7.4-rc3.log`). |
| in-tree amdgpu of 7.4-rc3 oopses in `amdgpu_atom_parse_data_header+0x30` during probe; no `/dev/dri` | Confirmed from `A1`; not retried. |
| 256G BAR0 needed the enlarged 64-bit PCI hole | Still in place (`pci-hole64-size=1024G` in the running QEMU command line); BAR0 assigns at `0x30000000000`, BAR2 at `0x34000000000`. |
| "no route to a working `/dev/dri`" | **Wrong.** There is one, and it is the route ktool's `dmabuf_test.py` uses. |

The route that works is the **stock image kernel `7.2.6-070206-generic`**,
for which the image's amdgpu **7.1.9 DKMS module is already built**
(`dkms status`), **plus the rocjitsu module options that `dmabuf_test.py`
writes and which were missing here**:

```
options amdgpu emu_mode=1 fw_load_type=0 discovery=2 ip_block_mask=0x7f \
                vm_update_mode=3 gpu_recovery=0 vramlimit=1024
```

(`ktool/ktool/dmabuf_test.py:464`, plus `/lib/firmware/amdgpu/ip_discovery.bin`,
which was already present and matches `ip_discovery.sha256`.)

Without those options the same module gets a long way — BAR0 assigned, IP
discovery read, "442368M of VRAM memory ready" — and then fails at the PSP
handshake:

```
amdgpu 0000:02:00.0: psp reg (0x16280) wait timed out, mask: 80000000, read: 0 exp: 80000000
[drm:psp_v15_0_8_ring_create] *ERROR* Failed to wait for trust OS ready for ring creation
amdgpu 0000:02:00.0: probe with driver amdgpu failed with error -22
```

(`A2`, full log). With the options, the probe completes:

```
amdgpu 0000:02:00.0: added peer-to-peer DMA memory 0x30000000000-0x3003fffffff
amdgpu 0000:02:00.0: AIS: registered 1024MB device memory
kfd kfd: added device 1002:75c1
[drm] Initialized amdgpu 3.65.0 for 0000:02:00.0 on minor 1
```

`/dev/dri/renderD128..135`, `/dev/dri/card1..8` and `/dev/kfd` appear (`A3`).
Note `amdgpu 0000:02:00.0: Runtime PM not available`, so the runtime-suspend
downgrade of `peer2peer` described in ernic-gpu-dmabuf.md's verification pass
cannot be what causes the result below.

**Caveat:** a failed amdgpu probe leaves the module unremovable —
`rmmod amdgpu` segfaulted and the module stayed "Device or resource busy".
Reboot the guest rather than retrying the load.

## How the test was built

- `test/src/gpu/devmem_tx_gpu.c` — a copy of `test/src/devmem_tx.c` with one
  new option, `-G <render node>` (and `-J` to ask for GTT instead of VRAM),
  which replaces the memfd+udmabuf buffer with an amdgpu BO:
  `DRM_IOCTL_AMDGPU_GEM_CREATE` (`AMDGPU_GEM_DOMAIN_VRAM`,
  `AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED`) → `DRM_IOCTL_AMDGPU_GEM_MMAP` +
  `mmap` (to write the test pattern) → `DRM_IOCTL_PRIME_HANDLE_TO_FD`.
  Raw ioctls, so it still builds `-static` and needs no libdrm. Everything
  else — bind-tx, `sendmsg(MSG_ZEROCOPY)` + `SCM_DEVMEM_DMABUF`, completion
  accounting, the receiver — is unchanged, so the comparison with the udmabuf
  runs is like for like.
- Driver: `ionic-mod.sh build/load ernic-dmtx1` rebuilt for the stock
  `7.2.6-070206-generic` kernel (v7.2.6 DKMS source + srq + p4 +
  `patches/ernic-v7.2.6`). `ionic.ko` loads; `ionic_rdma.ko` does not
  (symbol-version mismatch against the stock kernel) — irrelevant to Ethernet
  and devmem.
- Guest 2 was left on `7.2.15-devmem`: the receiver is plain TCP.
- Addresses `10.213.100.1N` (finding F9; 192.168.100.x is still broken by
  stack 2's docker network, re-checked today).

`-F` (the PFN dump) **must not** be used with `-G`: `/proc/self/pagemap` has
no PFN for a VRAM `PFNMAP` mapping and the program exits with
`page 0 not present`.

## Results

Control first, same kernel, same driver, **udmabuf** source — this also
establishes that devmem TX works on the stock 7.2.6 kernel, which the earlier
matrix never tested:

```
RESULT tx PASS sends=50 bytes=5000150 completed=50 copied=0 compl_errors=0 ... MiB/s=245.9
RESULT rx PASS bytes=5000150 expected=5000150 mismatches=0 mismatch_zero_bytes=0
```

GPU dma-buf source (`A6`):

```
gpu: created BO handle=1 domain=VRAM size=4194304
gpu: exported dma-buf fd=4 from /dev/dri/renderD128
gpu: filled 4194304 bytes through the CPU mapping
bind-tx ok: ifindex=4 dmabuf_id=2 size=4194304
RESULT tx PASS sends=50 bytes=5000150 completed=50 notifications=9 copied=0
       compl_errors=0 compl_gaps=0 short_sends=0 ... MiB/s=157.7
RESULT rx PASS bytes=5000150 expected=5000150 whole_sends=50 partial_send=no
       extra=0 mismatches=0 mismatch_zero_bytes=0
```

and a 6-second stream: `sends=2874 bytes=287408622 completed=2874 copied=0`,
0 completion errors, `DMA-API error_count during bind: 0`.

So `net_devmem_bind_dmabuf()` accepts an **amdgpu** exporter as readily as a
udmabuf one, the NIC gathers from it, and the bytes arrive correct.

## Which case this is: system pages, not a GPU BAR

Three independent pieces of evidence, all pointing the same way.

**1. The BO's domain (`A5`), captured while the NIC held the binding:**

```
== /sys/kernel/debug/dri/1/amdgpu_gem_info
pid 3712 command devmem_tx_gpu:
    0x00000001: 4194304 byte GTT pin count 1 exported as ino:6 CPU_ACCESS_REQUIRED VRAM_CLEARED
== /sys/kernel/debug/dma_buf/bufinfo
    Attached Devices: 0000:01:00.0        <- the ionic NIC
```

The BO was **created as VRAM** (`VRAM_CLEARED` is the residue of that) and is
reported as **GTT, pin count 1** once attached. amdgpu migrated it.

**2. The DMA addresses the driver writes into descriptors (`A4`).**
`kretprobe:ionic_tx_map_frag.constprop.0` returns exactly that address.
Over a 6-second GPU-sourced stream:

```
@frag_addr_hi32[256]: 75967          # every one of 75,967 frags, addr >> 32 == 0x100
@frag_addr_min: 1099641651323        # 0x100_07C0_xxxx
@frag_addr_max: 1099645841408
```

against the guest's address map:

| Range | What |
| --- | --- |
| `0x30000000000`–`0x3FFFFFFFFFF` | GPU BAR0 (256G), of which amdgpu registered `0x30000000000`–`0x3003fffffff` as peer-to-peer DMA memory |
| `0x10000000000`–`0x1017fffffff` | **System RAM** |

Every frag address is inside System RAM; **not one** is in the GPU BAR or in
the registered P2P range. (The guest has no IOMMU, so these are physical
addresses, not IOVAs — the same precondition the matrix's PFN check relies
on.)

**3. The emulator's counters.** `tx_dma_error` did not move, and the payload
verified — consistent with the NIC reading ordinary guest RAM. Had the
descriptors carried BAR addresses, the emulator could not have read them at
all: `rocm-ernic` resolves guest addresses only through
`vfu_addr_to_sgl()`/`vfu_sgl_get()`, which need a region with a host `vaddr`,
and QEMU sends no fd for a `ram_device_ptr` BAR — rocm-ernic's own docs say
"GPU VRAM buffers would need a vfio-user DMA proxy for GPU BAR regions, which
does not exist" (`docs/performance.rst`).

### Why, mechanically

On v7.2.x `net_devmem_bind_dmabuf()` uses the **static**
`dma_buf_attach()` + `dma_buf_map_attachment_unlocked()`, with no
`dma_buf_attach_ops`; `peer2peer` is set only from `importer_ops`, so it is
false. amdgpu's pin then strips VRAM from the allowed domains whenever any
attachment has `peer2peer == false`, and the map hands back page DMA
addresses for GTT. The GPU's `pci_p2pdma_distance()` and the registered P2P
range are irrelevant: nothing ever asks for peer-to-peer.

This is **not** an emulator limitation — the same code path on real hardware
with a real Instinct GPU gives the same answer on a v7.2.x kernel. Only
v7.4-rc's dynamic attach + pin with P2P allowed can keep the buffer in VRAM,
and that is the configuration §8 of
[real-hardware-runbook.md](real-hardware-runbook.md) describes.

## What was not done

- **The v7.4-rc3 leg** — the one that could have shown VRAM. It needs a
  working amdgpu on 7.4-rc3, which is exactly the blocker that stands: the
  7.1.9 DKMS will not compile against 7.4-rc3 and the in-tree 7.4-rc3 amdgpu
  oopses on probe against rocjitsu. Whether the ktool module options also fix
  the in-tree 7.4-rc3 probe was **not retried** — the options were missing in
  the earlier attempt too, so that attempt is not conclusive either. This is
  the single cheapest next experiment: boot 7.4-rc3-p2p, write
  `/etc/modprobe.d/amdgpu-rocjitsu.conf`, `modprobe amdgpu`. If it probes, the
  dynamic-attach path can be tested with `devmem_tx_gpu` unchanged. Even then
  the emulator cannot read a BAR, so the expected outcome is a bound buffer
  whose descriptors point at VRAM and a NIC that fails every DMA
  (`tx_dma_error`) — which is itself a useful, publishable result.
- `pci_p2pdma_distance()` between GPU and NIC was **not** measured directly
  (it needs a small kernel module); the `added peer-to-peer DMA memory` line
  shows only that the GPU registered a P2P range.
- The ROCm/HIP path (`hipMalloc` + `hsa_amd_portable_export_dmabuf`, which is
  what `ktool/userspace/amdgpu-dmabuf-io` uses) was not tried; the raw GEM
  ioctl path was enough and has fewer moving parts.
- No KASAN/lockdep leg: the stock 7.2.6 kernel has neither.
- No long run, no matrix cases, no jumbo, no TSO sweep from a GPU buffer.

## State left behind

Guest 1 was returned to `7.2.15-devmem` with the stock in-tree ionic after
this test (see the summary of the session). The amdgpu options file
`/etc/modprobe.d/amdgpu-rocjitsu.conf` was **left in guest 1** — amdgpu is
still blacklisted on the kernel command line, so it does not load by itself on
the devmem test kernels; remove the file if that is unwanted. `~/gputx/` in
guest 1 holds the built `devmem_tx_gpu`.

---

## CORRECTION (2026-11-19, evening)

Two statements above are **WRONG** and are retracted:

- *"Had the descriptors carried BAR addresses, the emulator could not have read
  them at all ... rocm-ernic's own docs say 'GPU VRAM buffers would need a
  vfio-user DMA proxy for GPU BAR regions, which does not exist'"* (§"Which case
  this is", point 3).
- *"Even then the emulator cannot read a BAR, so the expected outcome is a bound
  buffer whose descriptors point at VRAM and a NIC that fails every DMA
  (`tx_dma_error`)"* (§"What was not done", the v7.4-rc3 leg).

The v7.4-rc3 leg has since been run and the opposite happened: the NIC read
**VRAM**, through vfio-user P2P, and every byte verified at the peer. See
[test-gpu-vram.md](test-gpu-vram.md) and `test/evidence/gpu-vram/`.

What made the difference, beyond v7.4-rc3's `dma_buf_dynamic_attach()` +
`allow_peer2peer`:

- **Fork QEMU commit a2ee87e** (`sbates130272/qemu`, branch
  `dev/stephen/pci-mmio-bridge-submit`), "hw/vfio: assign region fd to RAMBlock
  for vfio-user P2P DMA", passes a vfio-user BAR's region fd in `DMA_MAP`, so a
  peer server maps the window. Stack 1 already runs it: both `rocm-ernic`
  servers map a 262144 MiB `/memfd:rocjitsu-vram` region.
- **`CONFIG_PCI_P2PDMA=y`**, which the kernels used for this run did not have
  (`7.4.0-rc3-devmem-kasan-p2p` does). Without it `pci_p2pdma_distance()` is a
  stub returning -1 and amdgpu clears `attach->peer2peer` unconditionally — so
  the GTT result recorded above was over-determined.

The rest of this document (the amdgpu/rocjitsu probe recipe, the test program,
and the v7.2.x analysis) stands.
