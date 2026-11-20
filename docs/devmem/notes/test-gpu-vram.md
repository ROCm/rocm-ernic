# (B) GPU VRAM as the devmem TX source — the decisive run

Run 2026-11-19 (evening) on the `ionic-devmem` stack (stack 1). Sender
`devmem-1` on `7.4.0-rc3-devmem-kasan-p2p`, receiver `devmem-2` on
`7.2.15-devmem` (plain TCP). Evidence under `test/evidence/gpu-vram/`
(`B1`–`B4`, `C0`–`C3`). **Nothing in the emulator was changed.**

## Answer

**YES. The emulated ionic NIC sources devmem TCP TX directly from GPU VRAM,
reading the GPU's BAR over vfio-user P2P, and the payload verifies byte for
byte at the peer.** 5,626,668,795 bytes in 30 s, 0 mismatches,
emulator `tx_dma_error` = 0.

This retracts the earlier conclusion in
[ernic-gpu-dmabuf.md](ernic-gpu-dmabuf.md) and
[test-gpu-dmabuf.md](test-gpu-dmabuf.md) ("the emulator cannot read another
device's BAR"); a dated correction has been appended to each, and to
[real-hardware-runbook.md](real-hardware-runbook.md).

## The decisive evidence (case C1 / C3, with C2 as the control)

| Probe | VRAM case (C1, C3) | GTT control (C2, `-J`) |
| --- | --- | --- |
| `pci_p2pdma_distance_many()` return | **4** | 4 |
| `amdgpu_dma_buf_attach()` → `attach->peer2peer` | **true** | true |
| `amdgpu_dma_buf_pin()` return | 0 | 0 |
| which map path ran | **`amdgpu_vram_mgr_alloc_sgt`** | `drm_prime_pages_to_sg` |
| `amdgpu_gem_info` while the binding is live | **`4194304 byte VRAM VISIBLE pin count 1`** | `4194304 byte GTT pin count 1` |
| `ionic_tx_map_frag()` return, bucketed by `addr >> 32` | **`[0x300]: 1480261`** (all) | `[0x100]: 275551` (all) |
| frag address range | `0x30003F86...`–`0x300403...` | `0x100...` |
| receiver | PASS, 0 mismatches | PASS, 0 mismatches |
| emulator `tx_dma_error` | 0 | 0 |

Guest address map (`C0`):

| Range | What |
| --- | --- |
| `0x30000000000`–`0x33fffffffff` | GPU (02:00.0) **BAR0**, 256 G |
| `0x34000000000`–`0x340001fffff` | GPU BAR2, 2 M |
| `0x10000000000`–`0x1017fffffff` | System RAM |
| `0xfe000000`/`0xfe400000` | NIC (01:00.0) BAR2/BAR0 |

So `0x300_xxxxxxxx` is unambiguously inside the GPU's BAR0 and
`0x100_xxxxxxxx` is unambiguously System RAM. The GTT control exists precisely
to show the probe tells them apart: same program, same driver, same kernel,
one flag (`-J`) difference, and the bucket moves from `0x300` to `0x100`.

Host side (`C1-ernic-server-maps.txt`): each `rocm-ernic` server maps a
**262144 MiB `/memfd:rocjitsu-vram`** region — the GPU's BAR0 window, handed
over by QEMU in `DMA_MAP` with its region fd. That is the mapping the server
resolves `0x300_xxxxxxxx` through.

## The chain, link by link

| Link | State | Evidence |
| --- | --- | --- |
| QEMU passes the GPU BAR's region fd to peer vfio-user servers | **OK** — fork QEMU commit `a2ee87e` (`sbates130272/qemu`, `dev/stephen/pci-mmio-bridge-submit`), already what stack 1 runs | `C1-ernic-server-maps.txt`: 256 GiB `/memfd:rocjitsu-vram` in both servers |
| Guest kernel has peer-to-peer | **OK** — `CONFIG_PCI_P2PDMA=y`, `CONFIG_ZONE_DEVICE=y` | `C0`, `B1` |
| `pci_p2pdma_distance(GPU, NIC)` ≥ 0 | **OK, = 4** | `C1-probe.txt` |
| amdgpu probes the emulated rocjitsu GPU on 7.4-rc3 | **OK, after two test-bed patches** (below) | `B4`, `/dev/dri/renderD128` |
| amdgpu runtime PM | not available → `pm_runtime_get_if_active()` returns < 0, amdgpu takes a plain reference and **keeps** `peer2peer` | `B4` ("Runtime PM not available"), `C1-probe.txt` |
| net core attaches with peer-to-peer | **OK** — v7.4-rc3 `net/core/devmem.c` uses `dma_buf_dynamic_attach()` with `.allow_peer2peer = true` plus `dma_buf_pin()` | source; `C1-probe.txt` |
| amdgpu pins in VRAM | **OK** | `C1-live-placement.txt` |
| ionic writes BAR addresses into TX descriptors | **OK** | `C1`/`C3-probe.txt` |
| emulator resolves them | **OK** — `tx_dma_error` 0, payload verified | `C3-*`, `C1-ernic1-stats.txt` |

### Why `pci_p2pdma_distance()` succeeds here

The two functions are under **different QEMU PCIe root ports** (NIC behind
`00:03.0`, GPU behind `00:04.0`), so there is no common upstream bridge and
`calc_map_type_and_dist()` falls through to `map_through_host_bridge`. The
guest's host bridge is `8086:29c0` (QEMU's 82G33), which is **not** in
`pci_p2pdma_whitelist[]`. It still succeeds because
`cpu_supports_p2pdma()` returns true for any AMD CPU of family ≥ 0x17, and the
guest CPU is `AuthenticAMD`, family 23 (the host is an AMD machine and the
vCPU model follows). The result is `PCI_P2PDMA_MAP_THRU_HOST_BRIDGE`,
distance 4 — not a bus-address mapping, but ≥ 0, which is all amdgpu's gate
asks for.

**This is load-bearing and fragile**: on an Intel host the same stack would get
`-1` and fall back to GTT. Anyone reproducing this needs an AMD Zen host, or
both devices under one root port, or an entry in the whitelist.

Note the GPU did **not** register p2pdma memory here (no "added peer-to-peer DMA
memory" line, no `p2pmem` sysfs directory — unlike the stock-7.2.6 run in
[test-gpu-dmabuf.md](test-gpu-dmabuf.md)). It does not need to:
`pci_p2pdma_distance_many()` does not require the provider to have published
p2pmem, and `amdgpu_vram_mgr_alloc_sgt()` maps the BAR with
`dma_map_resource()`.

## What had to change to get here

Nothing in rocm-ernic, nothing in QEMU, nothing in the ionic series. Two
**test-bed patches to amdgpu**, both in `test/gpu/patches/`, built into
`test/gpu/amdgpu-74/` and loaded as a replacement `amdgpu.ko`
(`test/gpu/amdgpu-74-mod/amdgpu.ko`, stripped; the original is kept in the
guest as `amdgpu.ko.intree-orig`):

1. `0001-drm-amdgpu-ras-skip-vbios-query-without-atom-context.patch` (written
   by the previous run): `amdgpu_ras_query_ras_capablity_from_vbios()` oopses
   in `amdgpu_atom_parse_data_header()` when there is no VBIOS.
   **Confirmed necessary**: the unpatched in-tree 7.4-rc3 amdgpu still oopses
   there *with* the rocjitsu module options set (`B2`), so the earlier
   "inconclusive because the options were missing" caveat is now settled — the
   options are not what that probe was missing.
2. `0002-drm-amdgpu-define-HAVE_ACPI_DEV_GET_FIRST_MATCH_DEV-in-tree.patch`
   (new): v7.4-rc3's amdgpu still guards `amdgpu_gmc_init_mem_ranges()` in
   `gmc_v12_0_sw_init()`, and `soc_v1_0_get_xcp_mem_id()`, with the **DKMS
   configure macro `HAVE_ACPI_DEV_GET_FIRST_MATCH_DEV`, which nothing in the
   kernel tree ever defines**. An in-tree build therefore leaves
   `adev->gmc.num_mem_partitions == 0` on GC 12.1 and the probe dies:

   ```
   amdgpu 0000:02:00.0: memory partition mode query is not supported
   amdgpu 0000:02:00.0: Invalid config, no compatible compute partition mode
                        found, available memory partitions: 0
   amdgpu 0000:02:00.0: hw_init of IP block <gfx_v12_1> failed -22
   ```

   Defining the macro in the two files that test it is enough. **This looks
   like a real bug in the 7.4 tree, not a test-bed artefact** — any in-tree
   (non-DKMS) amdgpu build is affected on gfx1250. Worth reporting.

Plus the existing rocjitsu module options from `ktool`'s `dmabuf_test.py:464`
(`/etc/modprobe.d/amdgpu-rocjitsu.conf`, already in guest 1), and the
`7.4.0-rc3-devmem-kasan-p2p` kernel, which was already built and installed.

With those, the probe is clean: `/dev/dri/card0` + `renderD128`,
`[drm] Initialized amdgpu 3.65.0 for 0000:02:00.0 on minor 0`, plus seven
`amdgpu_xcp_*` minors. `/dev/kfd` does **not** appear (kfd did not initialise);
the raw GEM + PRIME path does not need it.

## Throughput

Same plan (`-S 4m -P 1 -o 123 -l 100003 -a 7919`), `devmem_tx_gpu`:

| Case | Source | Seconds | Bytes | MiB/s |
| --- | --- | --- | --- | --- |
| C1 | **VRAM** | 8 | 1,268,938,067 | 151.3 |
| C2 | GTT (system RAM) | 6 | 1,047,431,422 | 166.4 |
| C3 | **VRAM** | 30 | 5,626,668,795 | **178.9** |

VRAM is not slower than system RAM here. Don't read much into the spread: this
is an emulated NIC whose "VRAM" is a host memfd, under a KASAN kernel, and the
run-to-run variation inside the VRAM cases (151 vs 179) is as large as the
difference between them.

## What was NOT run

- **No real GPU and no IOMMU.** "VRAM" here is a host `memfd` behind an
  emulated 256 G BAR; the guest has no IOMMU, so descriptor addresses are guest
  physical addresses, not IOVAs. Real hardware is still the only place to check
  real PCIe P2P routing, ACS, and an IOMMU translating BAR addresses — see
  [real-hardware-runbook.md](real-hardware-runbook.md) §8.
- **No ROCm/HIP path** (`hipMalloc` + `hsa_amd_portable_export_dmabuf`). The
  raw GEM ioctl path was used, as before.
- **No RDMA leg.** This is devmem TCP over the Ethernet datapath only; the
  `ib_write_bw --use_rocm_dmabuf` number in `qemu-minimal/AGENTS.md` is a
  separate (and independent) demonstration.
- **No matrix**: no jumbo MTU, no TSO/offload sweep, no XDP, no multi-queue, no
  teardown cases (`-X`), no `rmmod` while bound, no buffer larger than 4 MiB,
  and no run with more than 1 GiB of VRAM (`vramlimit=1024`).
- **No TSan/valgrind/sanitizer work** — nothing in `src/` was touched.
- **Only guest 1** was moved to the p2p kernel; guest 2 stayed on
  `7.2.15-devmem` as a plain TCP receiver.

## State left behind

- **Stack 1 is up and healthy.** Hash-verified `socat` transfer passes both
  ways over `10.213.100.1N` (`test/evidence/gpu-vram/Z-tcp-test.txt`).
  Finding F9 still holds: 192.168.100.x between stack 1's guests is broken by
  stack 2's docker subnet.
- **Guest 1 is left on `7.4.0-rc3-devmem-kasan-p2p`** (GRUB default) with:
  - the patched `amdgpu.ko` installed over the in-tree one at
    `/lib/modules/7.4.0-rc3-devmem-kasan-p2p/kernel/drivers/gpu/drm/amd/amdgpu/`
    (original kept beside it as `amdgpu.ko.intree-orig`); `depmod` has been run;
  - **amdgpu loaded**, `/dev/dri/renderD128` present. It is still
    `modprobe.blacklist=amdgpu` on the kernel command line, so it does **not**
    load by itself — a reboot leaves the GPU unbound until `modprobe amdgpu`.
    The blacklist was deliberately left in place: it keeps the boot safe while
    an unproven amdgpu is installed.
  - the patched ionic `dmtx74-1` **loaded** (a reboot returns to the in-tree
    one); `~/gputx/devmem_tx_gpu` rebuilt.
  - Go back with `scripts/install-kernel.sh 7.2.15 1` (or `7.4-rc3`).
- Guest 2: `7.2.15-devmem`, in-tree ionic, `~/gputx/devmem_tx_gpu` built.
- Server binary unchanged (`test/ernic/tx-offloads-1/build/rocm-ernic`).

### The warm-reset failure, again

An in-guest `reboot` of guest 1 on `7.4.0-rc3-devmem-kasan-p2p` **failed the
same way section 13 of [testbed.md](testbed.md) describes**: the console ends
at `shutdown[1]: Rebooting.` with no SeaBIOS banner. A cold
`stack.sh down && stack.sh up` recovered it, twice. Both observed occurrences
are on the `-p2p` kernel, which is the only configuration that registers the
256 G GPU BAR0 as a `ZONE_DEVICE`/p2pdma-capable region — a better suspect than
the earlier "one-off in the vfio-user reset path". **Operational rule for this
kernel: do not use an in-guest reboot; cold-cycle the stack.**

### Other things worth knowing

- A **failed** amdgpu probe still leaves the module unremovable (`rmmod`
  segfaults). Cold-cycle the stack rather than retrying.
- `bpftrace` sees `ionic_tx_map_frag.constprop.0`, not `ionic_tx_map_frag`;
  probing the plain name fails to attach.
- The stripped `amdgpu.ko` is 83 MB; the unstripped build artefact is 830 MB
  (KASAN + debug info) and is slow to copy into a guest. Strip it.

## Scripts and artefacts added

- `test/scripts/gpu/p2p-probe.bt` — the bpftrace probe set above.
- `test/scripts/gpu/run-vram-test.sh LABEL PORT SECONDS [sender args]` — one
  case end to end: receiver on guest 2, probe + sender on guest 1, placement
  captured while the binding is live, everything collected into
  `test/evidence/gpu-vram/`.
- `test/gpu/patches/0002-*.patch`, `test/gpu/amdgpu-74-mod/amdgpu.ko`.
