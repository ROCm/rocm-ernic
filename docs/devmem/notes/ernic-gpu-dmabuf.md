# Stretch goal: amdgpu dma-buf (rocjitsu VRAM) as devmem TX source for the emulated ionic NIC

Read-only study, 2026-11-18. No repository was modified.

Labels: **[V]** verified in source on this machine (file:line given);
**[I]** inferred from verified facts; **[M]** from memory of upstream Linux,
NOT verified (no kernel tree was available: `/var/tmp/ionic-devmem/linux` is
an empty clone, "No commits yet"; raw.githubusercontent.com is blocked).

Path abbreviations:

- `KT` = `/var/tmp/ionic-devmem/kernel-tools`
- `ERNIC` = `~/Projects/rocm-ernic` (tip of develop)
- `PIN` = `/var/tmp/ionic-devmem/rocm-ernic-pin` (61b934a3)
- `QM` = `~/Projects/qemu-minimal`
- `VFU` = `~/Projects/libvfio-user` (7e7a0a6)
- `QEMU` = `~/Projects/qemu` (branch
  `stephen-pci-mmio-bridge`, e9647d19669; NOT necessarily the build inside
  the `QEMU_IMAGE` container, see caveat in 2.4)
- `DKMS` = `/usr/src/amdgpu-6.14.14-2212064.24.04` (amdgpu DKMS source on
  this host; older than the 7.1.9 DKMS the test uses, same structure)

---

## 1. dmabuf_test.py: what it boots, attaches, proves, lacks

### Boot and devices [V]

- One VM, via `qemu-tool compose --stack vfio-user-rocjitsu-vm up --detach`
  (`KT/ktool/ktool/dmabuf_test.py:249-250`), env from `_compose_env()`:
  `VM_NVME=1`, vcpus 8, mem 16384 (`dmabuf_test.py:161-171`, `:643-646`).
- That stack is "rocjitsu-only ... No ernic NIC is attached"
  (`QM/qemu/compose/vfio-user-rocjitsu-vm/docker-compose.yml:2-6`). Two
  containers: `rocjitsu --config gfx1250_mi455x.json --vfio-socket
  /run/vfu/rocjitsu-1.sock` (`docker-compose.yml:47-52`) and a qemu container
  that runs `qemu-tool run-vm ... --vfio-userdev <all sockets> [--nvme
  $VM_NVME]` (`docker-compose.yml:108-117`).
- Devices in the guest:
  - GPU: `vfio-user-pci` on rocjitsu's socket, `rombar=0`,
    `x-msg-timeout=60000` (`QM/qemu/qemu-tool/run_vm.py:427-442`).
  - NVMe: QEMU's own emulated `nvme` + `nvme-ns` on a qcow2
    (`run_vm.py:345-409`). No `vram-dev=` is passed: that needs
    `cfg.vram_dev_index`, which dmabuf_test never sets (`run_vm.py:392-394`).
  - Guest RAM: `memory-backend-memfd,share=on` bound as the machine memory
    backend (`run_vm.py:493-501`), so vfio-user servers get an fd for RAM.
  - SLIRP user networking for SSH only (compose comment
    `docker-compose.yml:78-79`).
- Flow: boot stock kernel, build v7.2.4 + `patches/dmabuf-72x-backport`
  (`dmabuf_test.py:285-303`, `:649`), build amdgpu DKMS 7.1.9 against it
  (`:399-426`), install, write `options amdgpu emu_mode=1 fw_load_type=0
  discovery=2 ip_block_mask=0x7f vm_update_mode=3 gpu_recovery=0
  vramlimit=1024` and an `ip_discovery.bin` (`:463-467`), reboot, then patch
  rocjitsu's JSON so `local_mem_size` = `vram_aperture_bytes` = 8 GiB, i.e.
  "large BAR" (`:496-512`), `amdgpu-probe`, wait for `/dev/kfd`,
  `/dev/dri/renderD128`, an NVMe block device (`:518-538`).

### What it proves [V]

- Runs `userspace/amdgpu-dmabuf-io/test_p2p_io` against `/dev/nvme0n1` and
  against an XFS file, and greps for "Success" (`dmabuf_test.py:560-577`).
- `test_p2p_io`: `hipMalloc` 4 MiB, export with
  `hsa_amd_portable_export_dmabuf` (fallback `amdgpu_bo_alloc` in
  `AMDGPU_GEM_DOMAIN_VRAM` + `amdgpu_bo_export`), register the fd with
  io_uring (`IO_REGBUF_TYPE_DMABUF`, `target_fd` = the NVMe fd), one
  `read_fixed` of 4 MiB (`KT/userspace/amdgpu-dmabuf-io/test_p2p_io.cpp:63-91`,
  `:95-137`, `:204-245`).
- "Success" is `cqe->res >= 0` only (`test_p2p_io.cpp:260-266`). **It never
  reads the buffer back or compares data, and never checks where the BO
  lives** (VRAM vs GTT). So it proves: amdgpu on rocjitsu can export a
  dma-buf, nvme-pci can attach/map it, and a read into it completes. It does
  not prove the bytes landed in VRAM, nor that they are correct. [V for the
  code; I for the conclusion]

### What it lacks for an ERNIC two-endpoint network test

- No ernic server and no ionic device at all (stack is rocjitsu-only). The
  stacks that have both: `vfio-user-ernic-rocjitsu-vm` (single VM, plain
  `rocm-ernic -s /run/vfu/ernic-1.sock`, no TAP, no peer:
  `QM/qemu/compose/vfio-user-ernic-rocjitsu-vm/docker-compose.yml:56`) and
  `vfio-user-ernic-2vm` (hub + worker, L2 relayed between two VMs, rocjitsu
  optional per VM via profiles `rocjitsu-vm1/2`:
  `QM/qemu/compose/vfio-user-ernic-2vm/README.md:3-16`, `:44-50`). [V]
- No second endpoint (receiver), no IP config on `rocm-ernic0` (the 2vm
  README needs `vm-ernic.yml --tags configure` with distinct `vm_index`:
  `vfio-user-ernic-2vm/README.md:56-61`). [V]
- Kernel is v7.2.4 + io_uring dma-buf backport, with no ionic devmem patches
  and no ionic/ernic DKMS; `KT/ernic-pins.yml:5-11` pins the bundle kernel to
  v7.2.15 and ernic to 61b934a3, a different kernel from dmabuf_test's. [V]
- No netdev-side test program (ynl bind-tx + `sendmsg(MSG_ZEROCOPY,
  SCM_DEVMEM_DMABUF)`), no data verification, no placement check. [V: absent]
- `--skip-amdgpu-probe` exists "for CI without rocjitsu"
  (`dmabuf_test.py:658-659`), so CI may not exercise the GPU half at all. [V]
- The compose README for the combined stack still warns the guest amdgpu
  driver panics on probe and says to blacklist it
  (`vfio-user-ernic-rocjitsu-vm/README.md:66-76`); dmabuf_test works around
  that with the modprobe options above. Whether the README is stale is
  unverified.

---

## 2. Which DMA address the NIC gets, and can rocm-ernic resolve it

### 2.1 What amdgpu hands an importer [V in DKMS 6.14.14]

`amdgpu_dma_buf_map()` builds the sg_table by where the BO currently is
(`DKMS/amd/amdgpu/amdgpu_dma_buf.c:377-399`):

- `TTM_PL_TT` (GTT, system pages): `drm_prime_pages_to_sg()` +
  `dma_map_sgtable(attach->dev, ...)` (`:378-388`). DMA address = ordinary
  page DMA address. In a guest with no vIOMMU that is the guest-physical
  address of guest RAM. [I for the last sentence]
- `TTM_PL_VRAM`: `amdgpu_vram_mgr_alloc_sgt()` (`:390-396`), which computes
  `phys = cursor.start + adev->gmc.aper_base` and
  `dma_map_resource(dev, phys, ...)`
  (`DKMS/amd/amdgpu/amdgpu_vram_mgr.c:719-725`). DMA address = GPU BAR
  (framebuffer aperture) bus address + offset, with no struct page. In the
  guest: a GPA inside rocjitsu's VRAM BAR. This is the P2P case. [V; "GPA in
  the BAR" is I]

So the ionic driver would put either a guest-RAM GPA or a rocjitsu-BAR GPA
in the TX descriptor / SG element, unchanged (it just copies
`sg_dma_address`). Which one is decided by amdgpu, see section 3.

### 2.2 How the emulated NIC reads TX payload [V]

- `eth_tx_gather()` reads the head fragment and each SG element with
  `ernic_dma_read(emu->dma, emu->vfu_ctx, addr, ...)`
  (`ERNIC/src/ionic/ionic_eth_emu.c:1932-2015`, DMA calls at `:1961`,
  `:1994`, `:2007`; at the pin `PIN/src/ionic/ionic_eth_emu.c:1631`, `:1659`,
  `:1669`). A failure sets `IONIC_EMU_DROP_DMA` (`:1960`, `:1992`), counted
  as `tx_dma_error` (`ERNIC/src/ionic/ionic_emu_stats.c:95-96`,
  `ERNIC/docs/ionic.rst:405-406`).
- `ernic_dma_read()` -> `dma_xfer()` -> `dma_xfer_piece()`:
  `vfu_addr_to_sgl()` then `vfu_sgl_get()` then `memcpy`
  (`ERNIC/src/ernic_dma.c:81-82`, `:94`, `:104-108`; pin
  `PIN/src/ernic_dma.c:70`, `:81`). This is the only place that translates a
  guest address (`ERNIC/src/dma_range.h:36-38`). **This function is the code
  that decides.**
- The DMA callbacks only log; the server keeps no table of its own
  (`ERNIC/src/rocm_ernic_server.c:566-582`, setup at `:1973-1978`). The
  region table is entirely libvfio-user's, filled by QEMU's
  `VFIO_USER_DMA_MAP` messages.

### 2.3 What libvfio-user needs for that to work [V]

- `vfu_addr_to_sgl` succeeds if the address is inside any region the client
  mapped; otherwise `ENOENT` (`VFU/lib/dma.c:400-451`, `ENOENT` at `:444`).
- `vfu_sgl_get` -> `dma_sgl_get` returns `EFAULT` if the region has no
  `vaddr` (`VFU/lib/dma.h:288-289`). A region only gets a `vaddr` when the
  client passed an fd (`VFU/lib/dma.c:347-349`, `:365`; mmap at `:234`).
- libvfio-user has a message-based alternative for unmappable regions,
  `vfu_sgl_read()`/`vfu_sgl_write()` -> `VFIO_USER_DMA_READ/WRITE`
  (`VFU/lib/libvfio-user.c:2373-2396`, header comment
  `VFU/include/libvfio-user.h:735-738`). **rocm-ernic never calls them**
  (grep of `ERNIC/src` for `vfu_sgl_read|vfu_sgl_write`: no hits).

### 2.4 What QEMU tells the ernic server [V in local QEMU tree]

- The vfio listener maps a section if it is RAM or IOMMU; "RAM" includes
  `ram_device` regions (`QEMU/hw/vfio/listener.c:79-82`, ram_device handling
  at `:506`, `:598-609`, `:619-627`).
- An mmap-able BAR of another vfio/vfio-user device is exactly such a region:
  `memory_region_init_ram_device_ptr()` (`QEMU/hw/vfio/region.c:294`).
- For vfio-user the map message carries an fd only if
  `memory_region_get_fd(mr) != -1` (`QEMU/hw/vfio-user/container.c:89`,
  `:107-108`, `:124-127`, `:136-138`).
- Guest RAM is a shared memfd (`QM/qemu/qemu-tool/run_vm.py:493-501`), so it
  arrives with an fd and is mappable.

Consequences [I]:

| dma-buf backing | address in ionic descriptor | region in ernic's libvfio-user table | `ernic_dma_read` |
|---|---|---|---|
| GTT / system pages | guest RAM GPA | yes, with fd/vaddr | **works today, no emulator change** |
| VRAM, rocjitsu BAR is mmap-able in QEMU | GPA in GPU BAR | probably yes (ram_device section is DMA_MAPped) but fd = -1 (a `ram_device_ptr` RAMBlock has no fd) so vaddr NULL | `vfu_addr_to_sgl` ok, `vfu_sgl_get` -> EFAULT -> `-EIO` -> `tx_dma_error` |
| VRAM, rocjitsu BAR not mmap-able (pure MMIO) | GPA in GPU BAR | no region (non-RAM sections are skipped) | `vfu_addr_to_sgl` -> ENOENT -> `-EIO` -> `tx_dma_error` |

Not verified: whether rocjitsu's VRAM BAR is mmap-able (rocjitsu source is
not on this machine), and that `memory_region_get_fd()` is -1 for a
ram_device_ptr region (inferred from it being created from a pointer, not
read in `system/physmem.c`). The ernic debug log line "DMA region
registered: iova=... vaddr=..." (`ERNIC/src/rocm_ernic_server.c:568-570`)
would settle both in one boot: look for a region at the GPU BAR's GPA and
whether its vaddr is nil.

rocm-ernic already documents the same limitation for its own BAR:
"ernic_dma_read() cannot map BAR4 GPAs (PCI device memory, not guest DRAM)"
(`ERNIC/src/ionic/ionic_datapath.c:2000-2004`, `:3372-3376`); it solves it
only because it owns that memory (`dp->cmb_buf`).

### 2.5 What would have to change in the emulator (not done)

Any one of these, all centred on `dma_xfer_piece()` in `src/ernic_dma.c`:

1. **Message fallback**: when `vfu_sgl_get` fails with EFAULT (or
   `vfu_sg_is_mappable()` is false, `VFU/lib/libvfio-user.c:2399-2401`), use
   `vfu_sgl_read()` one sg at a time (it returns ENOTSUP for cnt != 1,
   `libvfio-user.c:2376-2378`). QEMU serves it with `pci_dma_read()`
   (`QEMU/hw/vfio-user/pci.c:87-110`, dispatch `:194-198`), which goes
   through QEMU's memory API and can reach another device's BAR. Needs a
   region to exist for the address (so only the "mmap-able BAR" row), and
   `vfu_sgl_read` asserts no request is pending
   (`libvfio-user.c:2375`). **Open question**: whether it can be called
   safely from ernic's doorbell path while QEMU is blocked in the BAR write
   that triggered it (possible deadlock); not analysed.
2. **Make the BAR region carry an fd**: QEMU would have to send the vfio-user
   region's fd for a ram_device section (a QEMU change in
   `vfio_user_dma_map`), then `ernic_dma_read` works unmodified.
3. **Side channel**: rocjitsu exports its VRAM backing (memfd) and the ernic
   server maps it and translates BAR GPAs itself, as it does for `cmb_buf`.
   Needs the BAR's GPA (it moves when the guest reprograms the BAR) and a
   security review: the address comes from the guest and must be bounded to
   the registered window (ERNIC AGENTS.md "Security").

### 2.6 Does the NVMe flow already solve the analogous problem? [V + I]

Yes, but in a way that does not transfer to a vfio-user server. The NVMe
controller in dmabuf_test is **QEMU's in-process model**, not a vfio-user
server, so it does DMA through QEMU's memory API rather than through a
`VFIO_USER_DMA_MAP` table:

- Stock path: `nvme_map_addr()` -> `qemu_sglist_add()` -> normal PCI DMA
  (`QEMU/hw/nvme/ctrl.c:973-982`). QEMU treats a ram_device region as
  non-direct ("we treat this as IO", `QEMU/include/system/memory.h:3136-3150`),
  so the access is bounced and dispatched to the owning device, i.e. it
  reaches rocjitsu's BAR whether via mmap or via vfio-user region
  read/write. [V for the two code facts; I that this is the path taken]
- The local fork additionally has a PoC fast path that recognises a VRAM
  address and adds the BAR's host pointer straight into the iovec
  (`QEMU/hw/nvme/ctrl.c:829-913`, used at `:935-966`). In this checkout the
  window is hard-coded `0xc400000000..0xc800000000` (`:841-852`) and requires
  the region to be RAM with a ram_block (`:896`). qemu-tool knows a newer
  `vram-dev=<id>,vram-bar=N` property (`QM/qemu/qemu-tool/caps.py:58`,
  `run_vm.py:392-394`, comment `:432-435`), but dmabuf_test does not pass it.
  Which variant the `QEMU_IMAGE` build has was not verified.

The fork's `pci-mmio-bridge` device is the generic statement of the same
problem: "VFIO can only map guest RAM not emulated PCI MMIO space"
(`QEMU/docs/system/devices/pci-mmio-bridge.rst:21-24`); it is a
register-sized command channel (`:44-56`), not a bulk data path.

---

## 3. net_devmem_bind_dmabuf + amdgpu exporter

### 3.1 Importer side [M, unverified]

- `netdev_nl_bind_tx_doit()` requires `netdev->netmem_tx`, then calls
  `net_devmem_bind_dmabuf(netdev, <dma dev = netdev parent>, DMA_TO_DEVICE,
  fd, ...)`.
- `net_devmem_bind_dmabuf()` uses `dma_buf_attach(dmabuf, dma_dev)` followed
  by `dma_buf_map_attachment_unlocked(attach, direction)`, then builds a
  genpool / `tx_vec` of net_iovs from `sg_dma_address/sg_dma_len` only (no
  struct page needed).
- `dma_buf_attach()` is the **static** attach: no `importer_ops`, so
  `attach->peer2peer` stays false and there is no move_notify /
  invalidate_mappings callback. The dma-buf core therefore pins the buffer
  (exporter `.pin`) for the life of the mapping.

Must be re-checked against the target kernel (v7.2.x) once a tree exists:
`net/core/devmem.c`, `net/core/netdev-genl.c`, `drivers/dma-buf/dma-buf.c`.

Contrast, verified: the NVMe importer in the backport is a **dynamic**
importer that opts in to P2P: `dma_buf_dynamic_attach(ctx->dmabuf, ctrl->dev,
&nvme_dmabuf_importer_ops, ctx)` with `.allow_peer2peer = true` and
`.invalidate_mappings`
(`KT/patches/dmabuf-72x-backport/0007-nvme-pci-implement-dma-buf-backed-requests.patch:95-97`,
`:218-219`, map at `:121`).

### 3.2 Exporter side [V in DKMS 6.14.14; 7.1.9 DKMS and mainline 7.2 not read]

- `.attach`: `amdgpu_dma_buf_attach()` clears `attach->peer2peer` when
  `pci_p2pdma_distance(adev->pdev, attach->dev, false) < 0` (and not XGMI)
  (`DKMS/amd/amdgpu/amdgpu_dma_buf.c:254-270`). It never fails the attach
  for a non-P2P importer; it only downgrades.
- `.pin`: `amdgpu_dma_buf_pin()` starts from `bo->allowed_domains` and
  removes VRAM if `CONFIG_DMABUF_MOVE_NOTIFY` is off **or any attachment has
  `peer2peer == false`**, then `amdgpu_bo_pin(bo, domains)` (`:281-312`).
  The comment names the intended VRAM user: "P2P with RDMA NICs without ODP"
  (`:289-296`).
- `.map_dma_buf`: if not pinned, validates into GTT, or GTT|VRAM only when
  `preferred_domains` has VRAM and `attach->peer2peer` (`:354-371`); then
  emits system-page or BAR addresses per 2.1.
- A user BO created VRAM-only gets GTT added to `allowed_domains`
  (`DKMS/amd/amdgpu/amdgpu_object.c:696-702`), so the GTT pin is legal for
  ordinary VRAM BOs (not for kernel or DISCARDABLE ones).
- Runtime PM: there is no `pm_runtime_*` call anywhere in
  `amdgpu_dma_buf.c` (grep, no hits). So the exporter takes no runtime-PM
  reference for an importer's mapping. [V] Whether that matters depends on
  the GPU supporting runtime suspend (PX/BOCO laptops/dGPUs do; a server
  Instinct normally does not) and is moot under rocjitsu with
  `emu_mode=1`. [I/M]
- kernel-tools' `amdgpu-no-large-bar/0001` adds: in the `TTM_PL_VRAM` case,
  if `real_vram_size > visible_vram_size`, warn "no large BAR — evicting BO
  to GTT for DMA-BUF P2P" and hand out system pages instead
  (`KT/patches/amdgpu-no-large-bar/0001-...patch` hunk at `@@ -408,6
  +432,29`; rationale `KT/patches/amdgpu-no-large-bar/README.md:40-54`).
  dmabuf_test does not apply it; it avoids the case by making the rocjitsu
  aperture equal to VRAM (`dmabuf_test.py:496-504`).

### 3.3 Result for a NIC importing through devmem [I]

With the static `dma_buf_attach` (3.1) the attachment has `peer2peer =
false` regardless of PCI topology, so `amdgpu_dma_buf_pin` strips VRAM and
pins the BO in **GTT**: amdgpu migrates the buffer contents to system pages,
and the netdev gets ordinary page DMA addresses. `pci_p2pdma_distance` is
never the deciding factor for devmem as it stands.

- Good news for the emulator: this is the "works today" row of 2.4. The
  ionic devmem TX PoC can be tested with an amdgpu dma-buf with **no
  emulator change**, and it does prove "TX from a GPU-owned dma-buf".
- Bad news for the stretch goal as worded: the NIC is not reading VRAM, in
  the emulator or on real hardware, until devmem becomes a dynamic,
  P2P-capable importer (`dma_buf_dynamic_attach` + `allow_peer2peer` +
  an invalidate callback that can quiesce in-flight TX), or amdgpu is
  patched to allow VRAM pins for a static importer.
- Side effects to expect: other attachments on the same dma-buf (e.g. the
  nvme one) lose VRAM placement too while the netdev binding exists, because
  the pin loop looks at all attachments (`amdgpu_dma_buf.c:300-302`). A KFD
  (hipMalloc) BO being moved to GTT and pinned triggers KFD eviction/restore
  of the owning process; interaction not analysed (the KFD code does
  special-case pinned BOs, `DKMS/amd/amdgpu/amdgpu_amdkfd_gpuvm.c:415`,
  `:3464`). A BO already pinned in VRAM (e.g. by another P2P importer) may
  make the GTT pin fail; error code not verified.

If devmem were made P2P-capable, then [M] `pci_p2pdma_distance()` between
GPU and NIC decides: same switch/root port is fine; across the host bridge
it needs the host-bridge whitelist or an AMD Zen CPU. In the VM both
vfio-user devices sit on the q35 root bus by default (`run_vm.py:427-442`,
bus only set with the switch option, `:446-489`), so the answer would depend
on the guest's CPU model (`-cpu host` on an AMD host passes; an Intel/TCG
model with the Q35 host bridge likely does not). The same caveat applies
today to the NVMe test: if the distance check fails in the guest, the "P2P"
test silently runs on GTT and still prints Success (section 1).

---

## 4. Proposed test procedure and observability

### 4.0 Prerequisite that blocks every devmem TX test on the emulator

The emulated LIF advertises no features: LIF identity "features ... left
zero, no offloads are emulated" (`ERNIC/src/ionic/ionic_eth_emu.c:1142`),
`LIF_SETATTR` only handles MAC and returns a zeroed completion for
everything else, including the features attribute (`:1525-1536`; same at
the pin, `PIN/src/ionic/ionic_eth_emu.c:1259-1268`), and the code relies on
it: "We advertise no offloads, so the driver never posts a TSO descriptor"
(`:1951-1953`). [V] The guest ionic driver derives `NETIF_F_SG` from the
negotiated `IONIC_ETH_HW_TX_SG` [M], so the guest netdev most likely has no
scatter-gather, and an skb with unreadable net_iov frags cannot be
linearized. Check `ethtool -k rocm-ernic0 | grep scatter` first. The
emulator's gather code already walks SG elements
(`ionic_eth_emu.c:1967-2012`), so advertising TX_SG is probably a small
change, but it is a change, and TSO must stay off (frame buffer bound at
`:1955`).

### 4.1 Emulated environment

Topology: `vfio-user-ernic-2vm` with profile `rocjitsu-vm1` (sender has GPU
+ ernic; receiver ernic only), guest kernel = ernic bundle kernel (v7.2.15,
`KT/ernic-pins.yml:5`) + ionic devmem TX series + amdgpu DKMS, amdgpu
options as in `dmabuf_test.py:464`, rocjitsu large-BAR config as in
`:496-504`.

Steps:

1. Baseline: devmem TX from a udmabuf / system-heap dma-buf, receiver does
   plain `recv` and checks a pattern. Establishes the ionic patch works.
2. GPU buffer: allocate with `amdgpu_bo_alloc` (VRAM domain, **without**
   `NO_CPU_ACCESS`, unlike `test_p2p_io.cpp:120`) or `hipMalloc`; fill with a
   per-run random pattern from the GPU side (HIP kernel or `hipMemcpy`
   host->device) so the data provably passed through the GPU allocation;
   export fd (`test_p2p_io.cpp:63-137` is the template).
3. `bind-tx` the fd to `rocm-ernic0` via netlink, `sendmsg` with
   `MSG_ZEROCOPY` + `SCM_DEVMEM_DMABUF`; reap the zerocopy completion from
   the error queue.
4. Receiver verifies the pattern byte for byte.
5. Mutation check: change the buffer via the GPU after bind (second
   `hipMemcpy`), send again, verify the receiver sees the new bytes (shows
   no stale copy was taken at bind time).

Observability, "where did the NIC read from":

- Guest, placement: `/sys/kernel/debug/dri/<n>/amdgpu_gem_info` shows each
  BO's current domain (VRAM/GTT/CPU), pin state and "exported as" [M:
  file name and fields]; `amdgpu_vram_mm`/`amdgpu_gtt_mm` usage before and
  after bind. Expected today: BO moves VRAM -> GTT at bind.
- Guest, the address itself: temporary `pr_debug`/tracepoint in the ionic
  TX map path printing the net_iov DMA address, or
  `/sys/kernel/debug/dma_buf/bufinfo` for the attachment list. Compare the
  address with `lspci -vv` BAR ranges of the GPU and with `/proc/iomem`
  "System RAM".
- Guest: `dmesg` for the no-large-BAR warning if that patch is applied
  (`amdgpu-no-large-bar/0001`), and p2pdma messages.
- ernic server: run with debug logging and record "DMA region registered:
  iova=... len=... vaddr=..." (`ERNIC/src/rocm_ernic_server.c:568-570`) to
  get the table of what the server can reach; `tx_dma_error`,
  `tx_desc_fetch_error`, `tx_desc_data_error` in the `--stats-file` "LIF
  Statistics" section or `ethtool -S` in the guest
  (`ERNIC/docs/ionic.rst:405-410`, `:425-429`); the `eth_tx` trace events
  (`docs/ionic.rst:592`).
- rocjitsu: if it can log BAR/VRAM accesses or expose its VRAM backing file,
  a VRAM read by a peer would be visible there only on the QEMU-mediated
  path; with today's GTT outcome rocjitsu sees nothing, which is itself the
  tell.

Decision rule: descriptor address inside "System RAM" + BO in GTT + data
correct = GTT path (expected). Address inside the GPU BAR + `tx_dma_error`
incrementing + nothing received = true P2P reached the emulator and the
emulator could not resolve it (section 2.5 work needed).

### 4.2 Real hardware (ionic/Pensando NIC + Instinct GPU)

Same program. Additional checks:

- `lspci -vv` GPU BAR0 size vs VRAM (large BAR), ACS state on the path,
  IOMMU on/off (with an IOMMU the DMA address is an IOVA, so compare via
  IOMMU debugfs or tracepoints rather than raw BAR ranges).
- `amdgpu_gem_info` domain of the BO during the bind, as above.
- Proof of a true VRAM read needs a bus-level or device-level witness:
  NIC firmware counters are not enough. Practical proxies: BO stays in VRAM
  and pinned while bound; descriptor address is in the GPU BAR; GTT/system
  memory usage does not grow by the buffer size; host memory bandwidth
  counters stay flat during a large send.
- Expected today: identical to the emulator, GTT pages, because of 3.3.

### 4.3 Expected failure modes

| Symptom | Cause |
|---|---|
| `bind-tx` -> EOPNOTSUPP | driver lacks `netmem_tx` (patch not loaded) [M] |
| send fails / packets dropped before the driver | no `NETIF_F_SG` on the emulated LIF (4.0) |
| bind fails with EINVAL/EBUSY | `amdgpu_dma_buf_pin` found no allowed domain (`amdgpu_dma_buf.c:308-309`) or BO already pinned elsewhere; `dma_map_sgtable` failure -> EBUSY (`:385-387`, `:403-406`) |
| bind succeeds, data correct, BO shows GTT | static attach, peer2peer false (3.3): not a VRAM read |
| `tx_dma_error` rising, nothing on the wire | BAR address reached ernic; EFAULT/ENOENT in `ernic_dma_read` (2.4) |
| stale or zero data at receiver | BO migrated after map without invalidate (static importers are pinned, so this would be an exporter bug), or buffer filled through a path that did not reach the BO |
| GPU process stalls at bind | KFD eviction/restore around the GTT migration (3.3), unverified |
| amdgpu probe panic in guest | stack README's known issue (`vfio-user-ernic-rocjitsu-vm/README.md:66-76`) if dmabuf_test's module options are not applied |
| real hw only: bind ok but VRAM beyond aperture | no large BAR; needs `amdgpu-no-large-bar` or ReBAR |

---

## Open questions

1. Confirm 3.1 in the target tree: is `net_devmem_bind_dmabuf` still a
   static `dma_buf_attach` in v7.2.x? Everything in 3.3 hangs on it.
2. Is rocjitsu's VRAM BAR mmap-able (fd-backed sparse mmap) in QEMU, and
   does QEMU's `VFIO_USER_DMA_MAP` to ernic include it (and with what fd)?
   One debug boot of ernic answers this.
3. Which QEMU NVMe VRAM variant is in the `QEMU_IMAGE` build (hard-coded
   window, `vram-dev`, or neither), and does the current NVMe test actually
   land in VRAM in the guest (p2pdma distance on q35 with the guest's CPU
   model)? `amdgpu_gem_info` during `test_p2p_io` would tell.
4. Can `vfu_sgl_read()` be used from ernic's doorbell context without
   deadlocking against QEMU's blocked BAR write?
5. KFD behaviour when an exported hipMalloc BO is pinned to GTT by a static
   importer; whether 7.1.9 DKMS / mainline 7.2 amdgpu differ from the
   6.14.14 source read here.
6. Does the ionic PoC intend to enable SG on the emulator (4.0), and is that
   tracked elsewhere in this workflow?

---

## Verification pass

Independent re-check, 2026-11-18. Nothing above this heading was edited.
Kernel sources that the first pass had only from memory were fetched
read-only at tag **v7.2.15** (the bundle's `kernel_tag`) from the GitHub
mirror `gregkh/linux` via `gh api .../contents/<path>?ref=v7.2.15`; cited
below as `K:<path>:<line>`. The in-guest amdgpu for `dmabuf_test` is still
the 7.1.9 DKMS, which was not available, so DKMS-vs-mainline differences
are called out. Everything else was re-opened at the cited file and line.

### Section 1 (dmabuf_test.py)

- CONFIRMED: one VM, stack `vfio-user-rocjitsu-vm`, `VM_NVME=1`
  (`dmabuf_test.py:161-171`, `:249-250`); stack has no ernic
  (`QM/.../vfio-user-rocjitsu-vm/docker-compose.yml:1-5`); v7.2.4 default
  (`:649`), `dmabuf-72x-backport` (`:286`), DKMS 7.1.9 (`:652`), module
  options (`:464`), large-BAR JSON patch (`:496-504`).
- CONFIRMED: `test_p2p_io` "Success" is `cqe->res >= 0` only
  (`test_p2p_io.cpp:260-266`); no readback, no placement check.
- CONFIRMED: no `vram-dev=` (needs `cfg.vram_dev_index`,
  `run_vm.py:391-393`); memfd `share=on` RAM (`run_vm.py:497-500`).
- WRONG (summary item 1, "lacks ... the bundle kernel"): misleading as
  worded. The bundle kernel is not a different feature set:
  `build_ernic_bundle.py:205-208` applies the same
  `patches/dmabuf-72x-backport` to v7.2.15, and
  `config-scripts/config-ernic-bundle` sources `config-amdgpu-dkms` and adds
  `IONIC`. The real differences are the base tag (v7.2.4 vs v7.2.15) and how
  the config is made: dmabuf_test starts from the guest's distro
  `/boot/config-*` + `localmodconfig` (`dmabuf_test.py:314-339`), the bundle
  from `make defconfig` + fragments (`build_ernic_bundle.py:211-215`).
- NEW (missed): because the bundle config is defconfig-based and no fragment
  in `config-scripts/` enables them (grep for
  `P2PDMA|ZONE_DEVICE|DEVICE_PRIVATE|UDMABUF|DMABUF_HEAPS`: no hits), and
  `K:arch/x86/configs/x86_64_defconfig` sets none of them, the bundle kernel
  most likely has `CONFIG_PCI_P2PDMA=n` (needs `ZONE_DEVICE`,
  `K:drivers/pci/Kconfig:206-208`), `CONFIG_UDMABUF=n` and no dma-buf heaps.
  Consequences [I, not checked against a built `.config`]:
  (a) `pci_p2pdma_distance()` is the stub returning -1
  (`K:include/linux/pci-p2pdma.h:106-110`, `:160-164`), so amdgpu clears
  `peer2peer` for every non-XGMI importer, including nvme: no VRAM P2P at
  all on the bundle kernel;
  (b) step 1 of 4.1 (udmabuf / system-heap baseline) needs a config change;
  (c) `-e HSA_AMD_SVM` in `config-amdgpu-dkms` is probably dropped by
  `olddefconfig` (depends on `DEVICE_PRIVATE`). `NET_DEVMEM` itself is
  `def_bool y` given `DMA_SHARED_BUFFER` + `PAGE_POOL`
  (`K:net/Kconfig:69-73`), so it should be on.
- NEW (missed): `patches/` also has `p2pdma/` (2019 GigaIO series,
  `pci_p2pdma_memremap`) and `knod/` (README only; KNOD RFC, includes "net:
  devmem: extend memory provider for knod" and amdkfd patches, mlx5e/bnxt
  only). Neither is applied by `dmabuf_test.py` or
  `build_ernic_bundle.py` (only `dmabuf-72x-backport` and, for DKMS,
  `amdgpu-72x-compat`: `dmabuf_test.py:286`, `:403`).

### Section 2 (address and resolution)

- CONFIRMED: amdgpu map emits page DMA addresses for `TTM_PL_TT` and
  `aper_base + offset` through `dma_map_resource` for `TTM_PL_VRAM`
  (`DKMS/amd/amdgpu/amdgpu_dma_buf.c:377-399`,
  `amdgpu_vram_mgr.c:719-725`).
- CONFIRMED: `dma_xfer_piece()` is the only guest-address translation in
  the server at both trees: `vfu_addr_to_sgl` + `vfu_sgl_get` appear only in
  `ERNIC/src/ernic_dma.c:81`, `:94` and `PIN/src/ernic_dma.c:70`, `:81`; no
  `vfu_sgl_read/write`, `vfu_map_sg` or other DMA API anywhere in `src/`.
- CONFIRMED: `eth_tx_gather()` DMA sites (`ERNIC/.../ionic_eth_emu.c:1961`,
  `:1994`, `:2007`; `PIN:1631`, `:1659`, `:1669`); `IONIC_EMU_DROP_DMA` ->
  `tx_dma_error` (`ERNIC/src/ionic/ionic_emu_stats.c:95-96`, name at `:326`;
  `docs/ionic.rst:405-406`).
- CONFIRMED: libvfio-user `ENOENT` for an unmapped address
  (`VFU/lib/dma.c:444`), `EFAULT` for a region without `vaddr`
  (`VFU/lib/dma.h:288-289`), `vaddr` only with an fd (`dma.c:347-365`).
  Extra: the register callback fires for fd-less regions too
  (`VFU/lib/libvfio-user.c:735-737`), so the debug-log probe in 2.4 does
  show a BAR region with `vaddr=(nil)` if QEMU maps one.
- CONFIRMED, and one "not verified" item is now verified: QEMU sends an fd
  only if `memory_region_get_fd() != -1`
  (`QEMU/hw/vfio-user/container.c:88`, `:107`); that returns
  `mr->ram_block->fd` (`QEMU/system/memory.c:2430-2437`), and a
  pointer-backed RAMBlock gets `fd = -1` (`QEMU/system/physmem.c:2552`). So
  a `ram_device_ptr` BAR section (`QEMU/hw/vfio/region.c:294`) reaches ernic
  without an fd. Row 2 of the 2.4 table stands. Whether rocjitsu's VRAM BAR
  is mmap-able is still UNVERIFIABLE here (no rocjitsu source).
- CONFIRMED, with stronger evidence the author missed: rocm-ernic states
  the limitation outright: "GPU VRAM buffers would need a vfio-user DMA
  proxy for GPU BAR regions, which does not exist"
  (`ERNIC/docs/performance.rst:232-235`), besides the BAR4 comments
  (`ionic_datapath.c:2000-2004`).
- CONFIRMED: NVMe in dmabuf_test is QEMU's in-process model; fork fast path
  with hard-coded `0xc400000000..0xc800000000`
  (`QEMU/hw/nvme/ctrl.c:841-852`, `:896`, `:935-966`); ram_device is
  non-direct (`QEMU/include/system/memory.h:3136-3150`).
- UNVERIFIABLE: which QEMU is inside `QEMU_IMAGE`; `vfu_sgl_read()` from the
  doorbell path (only the assert at `VFU/lib/libvfio-user.c:2375` checked).

### Section 3 (devmem + amdgpu)

- CONFIRMED (was [M], the claim everything in 3.3 hangs on):
  `net_devmem_bind_dmabuf()` uses the static
  `dma_buf_attach(binding->dmabuf, dma_dev)` then
  `dma_buf_map_attachment_unlocked()` (`K:net/core/devmem.c:229`, `:236`);
  no `dma_buf_attach_ops` in the file. `dma_buf_attach()` is
  `dma_buf_dynamic_attach(dmabuf, dev, NULL, NULL)`
  (`K:drivers/dma-buf/dma-buf.c:1084`) and `peer2peer` is set only from
  `importer_ops` (`:1048-1049`). A static attachment to an exporter with
  `.pin` is pinned at map time (`dma_buf_pin_on_map`, `:939-942`,
  `:1201-1202`). Open question 1 is closed: yes, still static in v7.2.15.
- CONFIRMED in mainline v7.2.15 as well as DKMS 6.14.14: pin strips VRAM if
  any attachment has `peer2peer == false`
  (`K:drivers/gpu/drm/amd/amdgpu/amdgpu_dma_buf.c:175-187`); attach only
  downgrades on `pci_p2pdma_distance() < 0` (`:100-102`); map allows VRAM
  only with `attach->peer2peer` (`:231-237`). So "devmem gets GTT pages from
  amdgpu, emulator and real hardware alike" holds.
- WRONG for the target kernel: "there are no runtime-PM calls in
  `amdgpu_dma_buf.c`". True of DKMS 6.14.14 (grep: none), false in mainline
  v7.2.15: `amdgpu_dma_buf_attach()` takes a runtime-PM reference for a
  peer2peer attachment with `pm_runtime_get_if_active()`, **downgrades to
  `peer2peer = false` if the GPU is runtime-suspended**, and
  `amdgpu_dma_buf_detach()` drops it
  (`K:.../amdgpu_dma_buf.c:104-117`, `:128-131`, `:142-152`). A
  non-peer2peer attachment (devmem today) takes no reference. Which
  behaviour the 7.1.9 DKMS has is UNVERIFIABLE.
- WRONG/outdated detail: the `CONFIG_DMABUF_MOVE_NOTIFY` leg of the pin
  rule. The symbol does not exist in v7.2.15 (`K:drivers/dma-buf/Kconfig`:
  no match) and mainline pin has no such test (`K:...:175-179`). The
  6.14.14 DKMS still has `if (!IS_ENABLED(CONFIG_DMABUF_MOVE_NOTIFY))
  domains &= ~VRAM` (`DKMS/amd/amdgpu/amdgpu_dma_buf.c:297-299`); if the
  7.1.9 DKMS kept it, on a 7.2 kernel every *pinned* attachment is forced to
  GTT regardless of `peer2peer` (`amdgpu-72x-compat` does not touch it; it
  only renames `.move_notify`, `0013-...patch`). Does not change the devmem
  result; does not affect the nvme dynamic importer, which is not pinned.
  UNVERIFIABLE for 7.1.9.
- NEW (mainline only): GFX12+ BOs with `AMDGPU_GEM_CREATE_GFX12_DCC` are
  never peer2peer (`K:...:96-98`). rocjitsu is gfx1250, so relevant only to
  DCC surfaces, not compute BOs.
- CONFIRMED: nvme backport importer is dynamic with `allow_peer2peer = true`
  (`0007-...patch:95-97`, `:218-219`). User VRAM-only BOs get GTT allowed
  (`DKMS/amd/amdgpu/amdgpu_object.c:697-702`).
- CONFIRMED (was [M]): p2pdma across the host bridge needs AMD family >=
  0x17 or a whitelisted bridge (`K:drivers/pci/p2pdma.c:522-528`,
  `:541-547`, `:778-779`); but see the `PCI_P2PDMA=n` point above, which
  preempts it on the bundle kernel.
- Imprecise: the DMA device is not literally "the netdev's parent"; bind-tx
  uses `netdev_queue_get_dma_dev(bind_dev, 0, NETDEV_QUEUE_TYPE_TX)`
  (`K:net/core/netdev-genl.c:1217-1222`). Same device for ionic unless the
  driver overrides it.
- UNVERIFIABLE: KFD eviction/restore when a hipMalloc BO is pinned to GTT.

### Section 4 (procedure, failure modes)

- CONFIRMED and stronger than "probably" (summary item 4): the emulator
  never grants `TX_SG`. LIF identity features zero
  (`ERNIC/.../ionic_eth_emu.c:1142`; `PIN:927`), `LIF_SETATTR` returns a
  zeroed features field (`ERNIC:1525-1536`; `PIN:1259-1268`), and the driver
  sets `NETIF_F_SG` only from `hw_features = cmd & comp`
  (`K:.../pensando/ionic/ionic_lif.c:1523-1524`,
  `:1612-1613`). rocm-ernic documents it: "advertises no checksum, TSO, or
  transmit scatter-gather offloads, so the guest stack always hands down
  linear skbs" (`ERNIC/docs/ionic.rst:361-363`,
  `docs/architecture.rst:1322-1323`).
- WRONG (failure table, "send fails / packets dropped before the driver"):
  the failure is earlier and specific. Without `NETIF_F_SG`,
  `tcp_sendmsg_locked()` leaves `zc != MSG_ZEROCOPY` and a send carrying a
  dmabuf id returns **-EINVAL** (`K:net/ipv4/tcp.c:1153-1156`,
  `:1172-1175`). No skb is built, nothing reaches the driver, and
  `bind-tx` itself still succeeds.
- CONFIRMED: `bind-tx` -> `EOPNOTSUPP` without netmem TX
  (`K:net/core/netdev-genl.c:1201-1206`).
- WRONG in the task framing / notes wording "requires `netdev->netmem_tx`"
  as a flag: in v7.2.15 `netmem_tx` is a three-state enum,
  `NETMEM_TX_NONE` / `NETMEM_TX_DMA` / `NETMEM_TX_NO_DMA`
  (`K:net/core/netdev-genl.c:1135-1138`, `:1201`;
  `K:net/core/dev.c:4054-4058`). The ionic PoC must set `NETMEM_TX_DMA`,
  not `true`.
- CONFIRMED: TX size is bounded by `sizeof(emu->frame)`
  (`ERNIC/.../ionic_eth_emu.c:1955`), so TSO must stay off.
- UNVERIFIABLE: `amdgpu_gem_info` field names; the stack README's amdgpu
  panic warning being stale
  (`QM/.../vfio-user-ernic-rocjitsu-vm/README.md:66-76` still says so).

---

## CORRECTION (2026-11-19, evening) — the emulator CAN read another device's BAR

**This document's conclusion that "the emulator cannot read a GPU BAR / would
need a vfio-user DMA proxy for GPU BAR regions, which does not exist" is
WRONG.** It has been disproved by experiment on stack 1; see
[test-gpu-vram.md](test-gpu-vram.md) and `test/evidence/gpu-vram/`.

Two things were wrong:

1. **The mechanism exists.** The fork QEMU
   (`sbates130272/qemu`, branch `dev/stephen/pci-mmio-bridge-submit`, commit
   **a2ee87e**, "hw/vfio: assign region fd to RAMBlock for vfio-user P2P DMA")
   gives a vfio-user device's BAR RAMBlock its region fd, so
   `memory_region_get_fd()` no longer returns -1 and QEMU passes the fd in
   `DMA_MAP` to peer vfio-user servers, which then map the window normally.
   Stack 1 already runs that QEMU: each `rocm-ernic` server maps a 262144 MiB
   `/memfd:rocjitsu-vram` region
   (`test/evidence/gpu-vram/C1-ernic-server-maps.txt`). See
   `qemu-minimal/AGENTS.md`, numbered issue 15, which also records
   `ib_write_bw --use_rocm=0 --use_rocm_dmabuf` at 548.89 MiB/s on the fork
   pair against `remote access error (10)` on stock QEMU.
2. **The kernels used for the earlier GPU runs had no `CONFIG_PCI_P2PDMA`**,
   so `pci_p2pdma_distance()` was the stub returning -1 and no importer could
   ever be granted peer-to-peer. On a kernel that has it
   (`7.4.0-rc3-devmem-kasan-p2p`), `pci_p2pdma_distance()` returns **4** between
   the emulated GPU (02:00.0) and the emulated NIC (01:00.0), amdgpu keeps
   `attach->peer2peer == true`, pins the BO in **VRAM**, and every TX frag DMA
   address the ionic driver writes into a descriptor is inside the GPU's BAR0
   window (`0x300_xxxxxxxx`, BAR0 = `0x30000000000`-`0x33fffffffff`). 5.6 GB
   streamed, zero mismatches at the peer, emulator `tx_dma_error` = 0.

The same wrong claim was in rocm-ernic's `docs/performance.rst` and has been
fixed there separately.
