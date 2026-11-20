# rocm-ernic emulated ionic Ethernet datapath: findings for devmem TCP TX PoC

Scope: read-only study. Two trees compared.

- **develop** = `~/Projects/rocm-ernic` (checked out `feat/devmem-poc`, same commit as
  `refs/heads/develop` and `refs/remotes/origin/develop`: `bd902345`). Paths below written `D:<path>:<line>`.
- **pin** = `/var/tmp/ionic-devmem/rocm-ernic-pin` at `61b934a3` (2026-10-28). Written `P:<path>:<line>`.
- Upstream Linux driver/core references were fetched from torvalds/linux master
  (`4405c18c95d2`, 2026-11-18) into `/tmp/ionic-ref/` (ionic_lif.c, ionic_txrx.c, ionic_if.h, ionic_dev.h,
  net/ipv4/tcp.c, net/core/dev.c). Written `K:<file>:<line>`. This is NOT the pinned guest kernel
  (v7.2.15 / ionic v7.2.6 per `kernel-tools/ernic-pins.yml:5,11`); anything about the driver is
  verified against master only and INFERRED for v7.2.x.
- libvfio-user = `~/Projects/libvfio-user` (master). `V:<path>:<line>`.
- QEMU = `~/Projects/qemu` (branch `stephen-pci-mmio-bridge`). `Q:<path>:<line>`.

Note on what the pin is: `ernic-pins.yml:7-8` describes `ernic_source_pin` as the commit "to use for
ionic/RDMA/P4 DKMS modules", and `ktool/ktool/build_ernic_bundle.py:276-316` builds an `ionic-ernic`
DKMS package from a kernel sparse clone plus rocm-ernic's patches. The *server* binary that a fleet
runs comes from a container image (e.g. qemu-minimal `vfio-user-ernic-2vm/docker-compose.yml:75`,
tag `ernic.8f35f4a`), which is neither develop nor the pin. OPEN: which server commit the devmem test
will actually run against.

---------------------------------------------------------------------------------------------------

## 0. Decisive facts (summary)

1. VERIFIED: the emulator offers **no** Ethernet offloads. LIF identity `features` is zero and
   LIF_SETATTR(FEATURES) returns a zeroed completion, so the driver computes `lif->hw_features = 0`;
   the netdev ends up with **no NETIF_F_SG, no NETIF_F_HW_CSUM, no TSO** (only NETIF_F_HIGHDMA).
   Same on develop and pin.
2. VERIFIED (kernel master): devmem TX cannot work on such a netdev. `tcp_sendmsg_locked()` only sets
   `zc = MSG_ZEROCOPY` when `sk->sk_route_caps & NETIF_F_SG`, and a send carrying a dmabuf id with
   `zc != MSG_ZEROCOPY` fails `-EINVAL` (`K:tcp.c:1159-1162,1178-1181`). Without NETIF_F_HW_CSUM the
   core falls back to `skb_checksum_help()` (`K:dev.c:4111-4133`), which returns `-EFAULT` on
   unreadable frags, so HW_CSUM is needed too.
3. VERIFIED: the emulator's TX gather **ignores the descriptor opcode, flags, csum_start/offset,
   hdr_len, mss and vlan_tci** completely. No L4 checksum is computed, no TSO segmentation, no VLAN
   insertion: bytes are copied from guest memory and written raw to the TAP.
4. VERIFIED: the emulator's TX scatter-gather parsing **disagrees with the real driver** about
   `desc->len`. The emulator treats `len` as the whole frame and derives the head length as
   `len - sum(sg lens)`; the driver puts the *first buffer's* length there. A devmem skb (small
   linear header + payload frags) would be dropped as `tx_desc_data_error`. Latent today because
   SG is never negotiated; same code on develop and pin.
5. VERIFIED: TX completions are always status 0, written synchronously in the doorbell handler,
   **even when the frame was dropped**. A MSG_ZEROCOPY completion therefore proves nothing about
   delivery.
6. VERIFIED: frames leave only through a host **TAP** (`--tap`), optionally claimed first by an
   in-process TX filter (S3, P4). The TCP mesh carries RDMA only. Guest-to-guest Ethernet = one TAP
   per server, joined by a host bridge.
7. VERIFIED: DMA reaches only regions QEMU registered with VFIO_USER_DMA_MAP **with an fd**
   (libvfio-user mmaps them). Guest RAM (memfd, share=on) qualifies; a udmabuf built from guest
   memfd pages is ordinary guest RAM and is readable (INFERRED for the guest-side DMA address).
   A BAR of another device is either not registered at all (ENOENT) or registered without an fd
   (EFAULT in `vfu_sgl_get`); either way `ernic_dma_read()` returns `-EIO`, the frame is silently
   dropped, `tx_dma_error` increments, and the guest still gets a successful TX completion.

So devmem TX against rocm-ernic needs **emulator changes** first (advertise TX_SG + TX_CSUM, fix SG
`len` semantics, implement CSUM_PARTIAL fill, ideally v1 SG descriptors), or the test passes/fails
for reasons unrelated to the kernel patch.

---------------------------------------------------------------------------------------------------

## 1. Features advertised / accepted, and what the guest netdev gets

### VERIFIED in rocm-ernic

- LIF_IDENTIFY: capabilities = ETH | RDMA (| P4). `eth.config.features` (offset 0x0a8) is left zero:
  "features is at 0x0a8; left zero, no offloads are emulated."
  `D:src/ionic/ionic_eth_emu.c:1124-1128,1142`; `P:src/ionic/ionic_eth_emu.c:910-913,927`.
- LIF_SETATTR: only `IONIC_LIF_ATTR_MAC` (3) is acted on; every other attr (including
  FEATURES, MTU, RSS, NAME, STATE) is accepted with `IONIC_RC_SUCCESS` and no state.
  The completion union is not written, so `comp.lif_setattr.features == 0`.
  `D:src/ionic/ionic_eth_emu.c:1525-1535`; `P:src/ionic/ionic_eth_emu.c:1259-1268`.
  The completion buffer is zero-initialised by the adminq service loop
  (`D:src/ionic/ionic_eth_emu.c:1888-1889`) and by the devcmd path (`D:...:889`).
- LIF_GETATTR: only MAC returned (`D:...:1537-1547`; `P:...:1270-1279`).
- Device-level IONIC_CMD_INIT/GETATTR/SETATTR: success, zeroed completion,
  "reads back as no features" (`D:...:928-936`; `P:...:734`).
- Q_IDENTIFY features = 0 (no CMB queues, no 2x/4x rings, no express doorbell)
  (`D:...:1624-1631`; `P:...:1335-1338`).
- Docs say the same: `D:docs/ionic.rst:361-366`, `D:docs/architecture.rst:1322-1323,1604-1606`.
- RSS key/indirection table accepted but ignored (`D:docs/architecture.rst:1604-1606`); RX queue
  picked by an internal flow hash (`D:docs/architecture.rst:1316-1319`).

### VERIFIED in upstream driver (master)

- `ionic_set_nic_features()` sends LIF_SETATTR(FEATURES) through the adminq and sets
  `lif->hw_features = cmd.features & comp.features` (`K:ionic_lif.c:1495-1525`). With comp = 0,
  `hw_features = 0`.
- `ionic_init_nic_features()` then sets only `netdev->features |= NETIF_F_HIGHDMA`; SG, HW_CSUM,
  RXCSUM, TSO*, VLAN, RXHASH are each gated on a `lif->hw_features` bit
  (`K:ionic_lif.c:1574-1643`; SG at 1615-1616, HW_CSUM at 1618-1619, TSO at 1622-1625).

### Result (INFERRED for the pinned v7.2.x driver; VERIFIED for master)

Guest `netdev->hw_features` and `features`: no `NETIF_F_SG`, no `NETIF_F_HW_CSUM`, no `NETIF_F_TSO*`,
no `NETIF_F_RXCSUM`. `ethtool -k` should show scatter-gather/tx-checksumming/TSO all off and fixed.
Identical on develop and pin.

---------------------------------------------------------------------------------------------------

## 2. TX descriptor handling

### Opcodes: none are decoded (VERIFIED)

`eth_tx_gather()` reads the 16-byte descriptor and uses exactly three things: `cmd >> 12` (addr),
`(cmd >> 8) & 0xf` (nsge), and le16 `len` at offset 8. The opcode nibble (bits 7:4), flags (3:0),
and the three trailing le16 words (vlan_tci, csum_start/hdr_len, csum_offset/mss) are never read.
`D:src/ionic/ionic_eth_emu.c:1932-2015`; `P:src/ionic/ionic_eth_emu.c:1604-1676`;
field macros `D:src/ionic/ionic_eth_emu_priv.h:378-380`, `P:...priv.h:322-325`.

Consequences:
- CSUM_NONE (0): works (pass-through).
- CSUM_PARTIAL (1): **not implemented**; frame is passed through with whatever is in the L4
  checksum field (the pseudo-header sum). No checksum is computed.
- CSUM_HW (2): same, pass-through.
- TSO (3): **not implemented**; each descriptor would be emitted as one raw frame without header
  replication, and the SOT/EOT flags are ignored. The comment at `D:...:1951-1953` says the
  emulator relies on never being sent one.
- VLAN flag / vlan_tci: ignored (no tag inserted).
- There is no checksum code left in the tree to reuse: CHANGELOG records the removal of
  "the unused packet parse and checksum helpers in src/net/net_headers.h" (`D:CHANGELOG.md:1363-1364`).

### Queue identity (VERIFIED)

- Version 0 only: `data[0]=0`, `supported=1` (bit 0), completion `ver=0`
  (`D:...:1605-1606,1640-1641`; `P:...:1322-1323,1347-1348`).
- desc_sz 16, comp_sz 16 (`D:src/ionic/ionic_eth_emu_priv.h:358-359`).
- TXQ: sg_desc_sz 128 (8 x 16), max_sg_elems 8, sg_desc_stride 8
  (`D:...:1613-1617`, `D:...priv.h:362-363`). v0 layout only; no `ionic_txq_sg_desc_v1` (256 B / 15).
- RXQ (develop): sg_desc_sz 128, max 8 (`D:...:1618-1621`, `D:...priv.h:368-372`).
- DIFFERENCE develop vs pin: at the pin Q_IDENTIFY read the qtype from `cmd[3]` (wrong byte; it is
  at `cmd[4]`), so every qtype got the adminq answer with no SG layout
  (`P:...:1292,1328-1333`); fixed on develop (`D:...:1567-1568`; `D:CHANGELOG.md:57-64`).
  Harmless for the Linux driver either way (below).
- Q_INIT: develop records `IONIC_QINIT_F_SG` in `q->sg` (`D:...:1748`); pin does not
  (`P:...:1417-1426`). Only the RX path uses it. TX SG gather is keyed on `nsge != 0` alone.

### What the driver does with that (VERIFIED on master)

- TXQ SG descriptor size: v1 (256 B) only if `qtype_info.version >= 1 && sg_desc_sz == 256`, else
  v0 128 B (`K:ionic_lif.c:1004-1008`). So against the emulator: v0, 8 elements per SG descriptor.
- BUT `ionic_lif_queue_identify()` overwrites `max_sg_elems = min(IONIC_TX_MAX_FRAGS - 1,
  MAX_SKB_FRAGS)` = 15 regardless of version (`K:ionic_lif.c:3982-3989`,
  `K:ionic_dev.h:213`, `K:ionic_if.h:1017-1025`), and `ionic_tx_descs_needed()` linearizes a
  non-GSO skb only when `nr_frags > q->max_sg_elems` (`K:ionic_txrx.c:1720-1723`).
  INFERRED: with SG enabled against a v0 device, an skb with 9..15 frags would be posted with
  nsge 9..15, the driver writing past the 8-element v0 SG slot, and the emulator dropping it
  (`nsge > TXQ_SG_MAX_ELEMS`, `D:...:1967-1969`, counted as tx_desc_data_error). A devmem-TX
  emulator change should therefore identify **TXQ version 1 with sg_desc_sz 256**.

### SG `len` semantics mismatch (VERIFIED, both trees) -- important

- Emulator: "len is the whole frame; the head descriptor carries the first fragment and the SG ring
  carries the rest"; `head_len = len - sg_total`; `sg_total >= len` is rejected as
  IONIC_EMU_DROP_DESC_DATA (`D:...:1951-1957,1981-1997`; `P:...:1622-1627,1648-1660`).
  The Python test client builds descriptors to that convention (`D:tests/py/ernic/eth.py:217`).
- Driver: `desc->len = cpu_to_le16(buf_info->len)` where `buf_info` is `bufs[0]`, the head mapping
  of `skb_headlen(skb)` bytes (`K:ionic_txrx.c:1130-1134,1575-1579`); ionic_if.h documents
  "@len: First data buffer's length, in bytes" (`K:ionic_if.h:917`). SG element lengths are separate
  (`K:ionic_txrx.c:1633-1645`).
- Effect if SG were negotiated: a typical devmem skb (headlen ~66, one or more payload frags totalling
  > 66 bytes) has `sg_total >= len` and is **dropped**; other shapes are silently truncated/garbled.
  Never hit today because without NETIF_F_SG the stack linearizes and nsge is always 0.

### Limits (VERIFIED)

- `len == 0` or `len > 9600` (sizeof emu->frame = IONIC_ETH_NET_MTU_MAX) -> drop DESC_DATA
  (`D:...:1955-1957`; `D:src/ionic/ionic_eth_emu_priv.h:535`; `D:src/ionic/ionic_eth_net.h:15`).
  No check that head + SG total fits in `emu->frame` beyond the "len is whole frame" assumption;
  if the `len` semantics are changed, a bound on the accumulated offset must be added.
- SG ring address: `sg_ring_base + index * 8 * 16` (`D:...:1973-1979`).

---------------------------------------------------------------------------------------------------

## 3. How TX buffers are read; which addresses are reachable

### Mechanism (VERIFIED)

- Every guest-memory access goes through `ernic_dma_read()/ernic_dma_write()`
  (`D:src/ernic_dma.h:1-22,68-71`).
- `dma_xfer_piece()` calls `vfu_addr_to_sgl(ctx, gpa, len, sg, 128, PROT_READ|PROT_WRITE)`, then
  `vfu_sgl_get()` to obtain host iovecs, `memcpy()`s, `vfu_sgl_mark_dirty()` on writes, `vfu_sgl_put()`
  (`D:src/ernic_dma.c:72-117`; `P:src/ernic_dma.c:63-100`). It is a **copy**; no long-lived mapping.
- A range spanning more than 128 host mappings is halved and retried (`D:src/ernic_dma.c:83-87,159-176`).
- Range wrap check first: `gpa <= UINT64_MAX - len` (`D:src/dma_range.h:40-43`) -> `-EINVAL`.
- It never uses `vfu_sgl_read()/vfu_sgl_write()` (the message-based VFIO_USER_DMA_READ/WRITE path,
  `V:lib/libvfio-user.c:2373,2386`), so memory the server has not mmap'ed is unreachable.
- DIFFERENCE develop vs pin: develop refuses every transfer with `-EPERM` unless the guest has Bus
  Master Enable set and the function is in D0 (`D:src/ernic_dma.c:146-148,224-230`), and holds TX
  doorbells until BME (`D:src/ionic/ionic_eth_emu.c:2033-2036`). The pin has no such gate
  (`P:src/ernic_dma.c:107-127`; no `bus_master` symbol in `P:src`).

### What libvfio-user can translate (VERIFIED)

- `dma_addr_to_sgl()` looks the IOVA up in the table of regions the client registered with
  VFIO_USER_DMA_MAP; unknown address -> `-1`, errno ENOENT (`V:lib/dma.h:238-267`,
  `V:lib/dma.c:399-450`). Write to a read-only region -> EACCES (`V:lib/dma.h:211-214`).
- A region registered **without an fd** has `vaddr == NULL` (`V:lib/dma.c:347-365`) and
  `dma_sgl_get()` fails EFAULT (`V:lib/dma.h:286-290`).

### What QEMU registers (VERIFIED in the local QEMU tree)

- The vfio listener maps every RAM section and skips non-RAM, protected, and bit-63 sections
  (`Q:hw/vfio/listener.c:79-91`).
- `vfio_user_dma_map()` passes `memory_region_get_fd(mr)`; only when that fd != -1 is an fd/offset
  sent (`Q:hw/vfio-user/container.c:83-140`). So guest RAM must be an fd-backed shared backend, as
  the documented command line does: `memory-backend-memfd,share=on` (`D:docs/usage.rst:275-277`).
- An mmap'ed BAR of a vfio/vfio-user device is a `ram_device` region created with
  `memory_region_init_ram_device_ptr()` (`Q:hw/vfio/region.c:294`, `Q:system/memory.c:1705-1721`);
  the listener does map those (page-aligned ones, `Q:hw/vfio/listener.c:598-612`), but they come from
  `qemu_ram_alloc_from_ptr()` and INFERRED carry `ram_block->fd == -1`, so the map is sent with no fd.
- QEMU's vfio-user client does implement server-initiated VFIO_USER_DMA_READ/WRITE
  (`Q:hw/vfio-user/pci.c:194-199`), which rocm-ernic does not use.

### Answers

- **Only guest RAM regions shared by QEMU?** Yes: anything registered with an fd. In practice the
  memfd guest RAM (VERIFIED mechanism). The ERNIC's own CMB (BAR4, `--cmb`) is handled separately via
  `ionic_datapath_set_cmb`, not through DMA (`D:src/ionic/ionic_eth_emu.c:201,407`).
- **udmabuf (memfd-backed guest pages)?** INFERRED yes. The pages are ordinary guest RAM; with no
  vIOMMU the DMA address the driver posts is their GPA, which lies inside the registered memfd region,
  so `ernic_dma_read()` copies them like any skb data. Nothing in the emulator distinguishes them.
  This makes udmabuf the right first devmem-TX test vector, but note it does **not** prove the
  "unreadable" property: the emulator can read it, and so could a buggy driver/stack that touched it.
- **Address inside another emulated device's BAR (e.g. a rocjitsu vfio-user GPU BAR)?** Not
  readable. Two cases, same outcome:
  1. BAR not mmap-able / trapped MMIO: not RAM, never DMA-mapped -> `vfu_addr_to_sgl()` returns -1
     (ENOENT) -> `dma_xfer_piece()` returns `-EIO` at `D:src/ernic_dma.c:88-90`.
  2. BAR mmap'ed by QEMU (ram_device): DMA-mapped without fd -> `vfu_addr_to_sgl()` succeeds,
     `vfu_sgl_get()` fails EFAULT -> `-EIO` at `D:src/ernic_dma.c:94-96`.
  Failure reporting: `eth_tx_gather()` returns 0 with `why = IONIC_EMU_DROP_DMA`
  (`D:src/ionic/ionic_eth_emu.c:1960-1963,1992-1997,2007-2010`); `eth_txq_service()` calls
  `ionic_emu_stats_drop(..., true, why)` (`D:...:2052-2054`), which increments the LIF counter
  `IONIC_LIF_TX_DMA_ERROR` (`D:src/ionic/ionic_emu_stats.c:95-96`). **No log line** is emitted on
  the TX drop path, and the guest still receives a normal TX completion with status 0
  (`D:...:2078-2093`). The guest sees it only as `tx_dma_error` in the LIF stats block the emulator
  publishes (driver folds it into `tx_aborted_errors`, `K:ionic_lif.c:1327-1330`; also the
  server's `--stats-file` "LIF Statistics" section, `D:src/ionic/ionic_eth_emu.h:291-297`). TCP
  would retransmit the same unreadable bytes forever.
  OPEN: whether the forked QEMU image used by `dmabuf_test.py`
  (`kernel-tools/ktool/ktool/dmabuf_test.py:49`, "...qemu-libvfio-user-sbates-fork") passes an fd for
  device BAR regions; if rocjitsu's BAR is fd-backed and that fd were forwarded, case 2 would become
  readable. Not verifiable from the trees given.

---------------------------------------------------------------------------------------------------

## 4. Where transmitted frames go; simplest plain-TCP setups; existing tests

### Path (VERIFIED)

`eth_txq_service()` per descriptor: gather -> optional fault injection -> registered TX filter gets
first refusal -> otherwise `ionic_eth_net_send()` to the host backend -> TX completion -> one IRQ
per service pass. With neither filter nor backend, TX is a sink (still completed).
`D:src/ionic/ionic_eth_emu.c:2039-2097`; `P:...:1678-1744`.

- Host backend = Linux TAP only: `open("/dev/net/tun")`, `IFF_TAP | IFF_NO_PI`, non-blocking,
  plain `write()`/`read()` of raw frames; **no IFF_VNET_HDR**, so no checksum/GSO metadata can be
  passed to the host (`D:src/ionic/ionic_eth_net.c:55-76,104-147`).
- Server option `-T/--tap IFNAME`, TAP must pre-exist and be owned by the server's user
  (`D:src/rocm_ernic_server.c:1167-1173,2007-2028,2097`; `P:src/rocm_ernic_server.c:1059,1132-1133,1583`).
- In-process endpoints via `ionic_eth_emu_register_tx_filter()` / `ionic_eth_emu_queue_rx_frame()`:
  S3 control plane and the P4 engine (`D:src/ionic/ionic_eth_emu.h:222-273`).
- The TCP mesh between servers carries RDMA only: "The mesh does not carry Ethernet"
  (`D:docs/architecture.rst:1337-1339`; `D:docs/ionic.rst:296-301`).
- RX: main loop drains the TAP and DMAs into posted RX buffers; `csum_flags` left 0 so the driver
  marks the skb CHECKSUM_NONE and the guest verifies in software
  (`D:src/ionic/ionic_eth_emu.c:2220-2233`; `P:...:1779-1787`).

### Simplest ways to get plain TCP

1. **One VM + host endpoint** (simplest): `ip tuntap add dev ernic0 mode tap user $USER;
   ip addr add 192.168.77.1/24 dev ernic0; ip link set ernic0 up`; run the server with
   `--tap ernic0`; address the guest's ionic netdev 192.168.77.2/24. Host stack is the TCP peer
   (`D:docs/ionic.rst:272-289`). The peer can then be a plain `nc`/`iperf3`/`ncdevmem`-style
   receiver on the host that validates payload.
2. **Two VMs**: one server + TAP per VM, both TAPs enslaved to one host bridge; distinct `--mac`
   per server (`D:docs/ionic.rst:291-326`, `D:docs/usage.rst:150-163`,
   `D:src/ionic/ionic_eth_emu.h:203-209`).

### Existing tests / tooling that wire guest Ethernet (VERIFIED)

- `ansible/roles/ernic_host_setup/tasks/tap.yml` creates the bridge and per-instance TAPs
  (`D:ansible/roles/ernic_host_setup/tasks/tap.yml:5-10,59-61`); the service launcher passes
  `--tap ${ERNIC_TAP_PREFIX}n` (`D:docs/service.rst:195-203`).
- `ansible/playbooks/sanity-tests.yml`: ping both directions between the two VMs
  (`D:ansible/playbooks/sanity-tests.yml:82-114`).
- `ansible/playbooks/tcp-performance-tests.yml`: iperf3 VM-to-VM over the TAP bridge
  (`D:ansible/playbooks/tcp-performance-tests.yml:6,119-150`); documented result 2.7 Gbit/s
  (`D:docs/performance.rst:176`).
- System-test workflow: `.github/workflows/ernic-system-tests.yml` and lanes
  (`D:docs/testing.rst:601,624,703-709`).
- `tests/test_ionic_ci.sh` test 8 only checks that `--tap` attaches (`D:tests/test_ionic_ci.sh:156-183`).
- Unit/Python tests drive the TX ring with a synthetic vfio-user client
  (`D:tests/py/ernic/eth.py`, `D:tests/test_ionic_queues.c`), using the emulator's own `len`
  convention.
- qemu-minimal `vfio-user-ernic-2vm`: a `tapsetup` container creates `tap0` and bridges it to a
  shared docker L2; each ernic runs `rocm-ernic -s ... -b tcp:manager|worker ... -m MAC -T tap0`
  (`qemu-minimal/qemu/compose/vfio-user-ernic-2vm/tapsetup/setup.sh:1-22`,
  `.../docker-compose.yml:72-98,129-157`). There is also `vfio-user-ernic-rocjitsu-vm`
  (ERNIC + rocjitsu GPU in one VM), the natural stack for a GPU-BAR dmabuf experiment.
- kernel-tools `dmabuf_test.py` currently targets rocjitsu GPU + NVMe P2P, not ERNIC Ethernet
  (`kernel-tools/ktool/ktool/dmabuf_test.py:1,209,249`).

---------------------------------------------------------------------------------------------------

## 5. Emulator limitations that would make devmem TX tests fail or pass vacuously

Fail outright (as the emulator stands, both trees):

- F1. No NETIF_F_SG / HW_CSUM in the guest (section 1) -> `sendmsg(MSG_ZEROCOPY + SCM_DEVMEM_DMABUF)`
  returns `-EINVAL` before the driver is involved (`K:tcp.c:1159-1162,1178-1181`). Setting
  `netdev->netmem_tx` alone in the driver is not enough. With SG but no HW_CSUM, TCP's
  CHECKSUM_PARTIAL skbs go to `skb_csum_hwoffload_help()` -> `skb_checksum_help()`, which returns
  `-EFAULT` for an unreadable skb and the packet is dropped (`K:dev.c:4111-4133,4254-4262`). So the emulator must grant IONIC_ETH_HW_TX_SG **and** IONIC_ETH_HW_TX_CSUM
  in the LIF_SETATTR(FEATURES) completion (or the PoC must force them in the driver, which then
  hits F2/F3).
- F2. CSUM_PARTIAL not implemented (section 2). If HW_CSUM is forced on, every TCP segment reaches the
  TAP with an incomplete L4 checksum; the peer (host stack, or a second guest whose RX is
  CHECKSUM_NONE) discards it. Connection never establishes data transfer.
- F3. SG `len` semantics mismatch (section 2) -> frames with frags dropped as `tx_desc_data_error`
  or garbled.
- F4. v0 SG (8 elems) while the driver believes 15 (section 2) -> skbs with 9..15 frags dropped;
  devmem skbs can have up to MAX_SKB_FRAGS frags.
- F5. TSO not implemented. If TSO bits were granted, each TSO descriptor would go out as a raw
  frame. Leave TSO ungranted: devmem TX does not require it, at the cost of MSS-sized skbs.

Pass vacuously / mislead:

- V1. TX completion is unconditional and immediate, status always 0, even for dropped frames
  (`D:src/ionic/ionic_eth_emu.c:2048-2096`). The MSG_ZEROCOPY errqueue notification and
  `tx_packets` on the guest advance regardless. Verify payload at the receiver.
- V2. No checksum verification anywhere in the emulator, and none on RX hand-off to the guest other
  than the guest's own software check. A host-TAP peer does verify (no vnet header).
- V3. Drops are silent: no log on the TX drop path; only LIF counters (`tx_dma_error`,
  `tx_desc_data_error`, `tx_desc_fetch_error`) and the `--stats-file`. Check them before/after.
- V4. udmabuf is readable guest RAM, so a test with udmabuf passes even if some path linearizes or
  copies the "unreadable" payload in the guest. It tests plumbing, not the unreadability contract.
- V5. Without `--tap` (or with the TAP missing in the service launcher, which is only a warning:
  `D:docs/ionic.rst:316-320`) TX is a sink that still completes everything.

Other constraints:

- MTU: LIF max_frame = `link.mtu_max` + 18; default profile mtu_max 9198, hard cap 9600-18
  (`D:src/ionic/ionic_eth_emu.c:1153-1156`, `D:src/ionic/ionic_profiles.c:43`,
  `D:src/ionic/ionic_profile.h:62-69`). TX frame buffer 9600 bytes. Jumbo over the TAP needs the
  TAP and bridge MTU raised (`D:CHANGELOG.md:55-56`).
- DIFFERENCE, RX at the pin: only the descriptor's first buffer is used; no RX SG. Frames larger than
  that buffer are dropped (`P:src/ionic/ionic_eth_emu.c:1757-1777`), which per the changelog happened
  "even at an MTU of 1500 ... once the driver had reused part of a buffer's page", and jumbo RX never
  worked (`D:CHANGELOG.md:44-56`). Develop spills into the RX SG list
  (`D:src/ionic/ionic_eth_emu.c:2129-2245`). A pin-era server as the **receiving** side of a TCP test
  can therefore lose full-size segments; prefer a develop-era server or a host-TAP receiver.
- Bus mastering: develop requires BME + D0 for all DMA; pin does not (section 3).
- TX servicing is synchronous on the main vfio-user thread, bounded by ring depth per doorbell
  (`D:...:2048`); one MSI-X per service pass. Throughput is low (documented 2.7 Gbit/s), so size
  tests accordingly.
- IOMMU/vIOMMU: the documented QEMU command line has no vIOMMU (`D:docs/usage.rst:270-286`); DMA
  address == GPA is assumed in everything above. QEMU's vfio listener does handle IOMMU regions
  (`Q:hw/vfio/listener.c:79-83`), so INFERRED a vIOMMU would work through translated DMA_MAPs, but
  nothing in rocm-ernic's tests exercises it. OPEN.
- `--cmb`: with a CMB the driver may place queues in BAR4; the emulator offers no CMB Ethernet
  queues (`D:src/ionic/ionic_eth_emu.c:1624-1630`), so not relevant unless enabled.
- The Ansible/DKMS patch set does not touch ionic Ethernet offloads
  (`D:ansible/files/patches/` contains only the RDMA AH patch, p4, rdma-core, srq).

---------------------------------------------------------------------------------------------------

## 6. develop vs pin, at a glance

| Area | pin 61b934a3 | develop bd902345 |
| --- | --- | --- |
| LIF features / SETATTR | none; comp zero | same |
| TX opcode handling | ignored; pass-through | same |
| TX SG parse (v0, 8 elems, `len` = whole frame) | yes | same |
| Q_IDENTIFY qtype byte | `cmd[3]` (wrong; all qtypes answered as adminq) | `cmd[4]`; unknown qtype refused |
| RX SG / jumbo RX | no (first buffer only) | yes (8-elem SG list) |
| DMA gated on Bus Master + D0 | no | yes (`-EPERM`) |
| Frame sink | TAP + TX filter | same |

---------------------------------------------------------------------------------------------------

## 7. Open questions

1. Which rocm-ernic server commit does the devmem test fleet run (container tag `ernic.8f35f4a` in
   qemu-minimal vs pin vs develop)? The pin governs the guest DKMS driver, not obviously the server.
2. Guest driver is DKMS-built from kernel ref v7.2.6 + rocm-ernic patches
   (`build_ernic_bundle.py:286-316`); the PoC series must land in that build (or replace it with the
   in-tree driver). Driver statements here were checked against master only.
3. Is changing the emulator in scope? Minimum: grant TX_SG + TX_CSUM in LIF_SETATTR(FEATURES) and
   LIF identity, fix head `len` semantics, fill L4 checksum for CSUM_PARTIAL from
   csum_start/csum_offset, identify TXQ v1 (256-byte SG, 15 elems), bound the gathered length.
   Alternative for a quick PoC: keep the emulator and hack the driver, but F2-F4 still apply.
4. For a true unreadable source (rocjitsu GPU BAR dmabuf): the ERNIC server cannot read it today.
   Options are message-based `vfu_sgl_read()` (QEMU supports VFIO_USER_DMA_READ) if QEMU can service
   reads of a ram_device region, or fd-passing for device BARs in the forked QEMU. Unverified.
5. Upstream master now has `enum netmem_tx_mode { NETMEM_TX_NONE, NETMEM_TX_DMA, NETMEM_TX_NO_DMA }`
   (netdevice.h) and `validate_xmit_unreadable_skb()` checks it (`K:dev.c:4169-4197`); v7.2.x may still
   have the boolean. Check the target tree before writing the patch.
6. vIOMMU behaviour untested.

---------------------------------------------------------------------------------------------------

## Verification pass

Independent re-check of the load-bearing claims (2026-11-18). Sources re-opened: develop `D:`, pin `P:`,
libvfio-user `V:`, QEMU `Q:`, the author's `/tmp/ionic-ref/` (`K:`), plus torvalds/linux master
`net/core/sock.c`, `net/core/skbuff.c`, `include/linux/netdev_features.h`, `net/core/netdev-genl.c`
fetched into `/tmp/ionic-ref-verify/` (`K2:`). Nothing above this section was edited.

### WRONG

- **W1. "sendmsg with a dmabuf id returns -EINVAL before the driver is involved" (section 0 item 2,
  section 5 F1, summary item 1).** WRONG for TCP on master. `sk_setup_caps()` forces
  `sk_route_caps |= NETIF_F_GSO` for every TCP socket, then `|= NETIF_F_GSO_SOFTWARE`, and when
  `sk_can_gso()` holds (the normal case: no xfrm header, GSO not disabled) sets
  `sk_route_caps |= NETIF_F_SG | NETIF_F_HW_CSUM` regardless of `dev->features`
  (`K2:sock.c:2676-2691`). So the test at `K:tcp.c:1159-1160` passes, `zc = MSG_ZEROCOPY`, and the
  `-EINVAL` at `K:tcp.c:1178-1181` is not taken on a device without NETIF_F_SG. The sendmsg
  **succeeds** (given the driver sets `netmem_tx` so that bind-tx and `net_devmem_get_binding()`
  succeed; `K2:netdev-genl.c:1241`). The failure moves to transmit time, in `validate_xmit_skb()`:
  a non-GSO skb with frags on a non-SG device goes to `__skb_linearize()` (`K:dev.c:4246-4248`),
  and `__pskb_pull_tail()` refuses an unreadable skb (`K2:skbuff.c:2898-2906`), so the skb is freed;
  a GSO skb goes to software segmentation with `sg == 0`, which copies payload
  (`K2:skbuff.c:4906,5068-5089`) and INFERRED fails or WARNs on unreadable frags. Net effect:
  **sendmsg returns success, every data segment is dropped in the guest stack before
  `ndo_start_xmit`, TCP retransmits forever.** The conclusion (emulator must grant TX_SG and TX_CSUM)
  stands, but the observable symptom is a stalled connection and qdisc/tx drop counts, not an errno.
  The `skb_checksum_help()` `-EFAULT` part is CONFIRMED (`K:dev.c:3679-3681,4110-4134,4254-4262`).
- **W2. "There is no checksum code left in the tree to reuse" (section 2).** WRONG. The RFC 1071
  helpers `net_csum_add()` / `net_csum_fold()` are in `D:src/net/net_headers.h:249-285` (pin:
  `P:src/net/net_headers.h:172-205`) and are used by `src/ionic/ionic_datapath.c` and
  `src/s3/s3_tcp.c`. The changelog entry (`D:CHANGELOG.md:1363-1364`) removed only the *unused*
  parse/checksum helpers. A CSUM_PARTIAL fill can reuse these.
- **W3. `vfu_addr_to_sgl(..., PROT_READ|PROT_WRITE)` (section 3).** WRONG in both trees. The
  protection is per direction: `write ? PROT_WRITE : PROT_READ` (`D:src/ernic_dma.c:81-82`),
  `wbuf ? PROT_WRITE : PROT_READ` (`P:src/ernic_dma.c:71`). Consequence: a TX read from a region
  QEMU mapped read-only (and fd-backed) succeeds; only writes get EACCES (`V:lib/dma.h:208-212`).

### CONFIRMED

- C1. LIF identity `features` left zero; LIF_SETATTR acts only on MAC and returns a zeroed
  completion, on both the devcmd and adminq paths (`D:ionic_eth_emu.c:889,953-955,1115,1142,
  1525-1535,1863-1865,1888-1889`; `P:ionic_eth_emu.c:927,1259-1268`). No option or profile gates this.
- C2. Driver: `hw_features = cmd & comp` (`K:ionic_lif.c:1523-1525`); `netdev->features` gets only
  HIGHDMA plus bits gated on `lif->hw_features` (`K:ionic_lif.c:1604-1641`). Guest netdev has no
  SG/HW_CSUM/TSO. (Master only; v7.2.x UNVERIFIABLE here.)
- C3. `eth_tx_gather()` reads only addr, nsge, `len`; opcode, flags, vlan_tci, csum_start/offset,
  hdr_len, mss are never read; identical at the pin (`D:...:1932-2015`; pin same code).
- C4. `len` semantics mismatch: emulator `head_len = len - sg_total`, drop if `sg_total >= len`
  (`D:...:1981-1993`); driver writes `desc->len = skb_headlen` (`K:ionic_txrx.c:1130-1134,1579,1617`);
  the Python test client follows the emulator's convention (`D:tests/py/ernic/eth.py:215-217`).
- C5. Q_IDENTIFY answers version 0 only; TXQ SG 8 x 16 = 128 bytes (`D:...:1605-1617,1640-1641`;
  `D:ionic_eth_emu_priv.h:362-363`). Driver picks v0 SG size (`K:ionic_lif.c:1004-1008`) but
  overrides `max_sg_elems` to 15 (`K:ionic_lif.c:3982-3989`, copied to the queue at `:616`) and
  linearizes only above that (`K:ionic_txrx.c:1720`). The "9-15 frags dropped" outcome stays
  INFERRED (`nsge > 8` drop at `D:...:1967-1969` is confirmed).
- C6. TX completion is written unconditionally with status 0 after any gather outcome; drops only
  bump counters; no `vfu_log` on the TX drop path or in `ionic_emu_stats.c`
  (`D:...:2048-2096`; `D:ionic_emu_stats.c:75-96`). On develop, with BME clear the doorbell is
  recorded but not serviced, so no completion until BME (`D:...:2033-2036`).
- C7. DMA is `vfu_addr_to_sgl` + `vfu_sgl_get` + memcpy; failure of either is `-EIO`
  (`D:ernic_dma.c:81-96`). Unregistered address: ENOENT (`V:lib/dma.c:440-443`); region without fd
  has no vaddr and `dma_sgl_get()` gives EFAULT (`V:lib/dma.h:286-288`, `V:lib/dma.c:356-380`).
  `vfu_sgl_read/write` are not used anywhere in `src/`.
- C8. QEMU: listener skips non-RAM sections (`Q:hw/vfio/listener.c:79-91`), maps page-aligned
  ram_device sections (`:598-612`); `vfio_user_dma_map()` sends an fd only when
  `memory_region_get_fd() != -1` (`Q:hw/vfio-user/container.c:88,104-106,121-137`); vfio region
  mmaps are `ram_device_ptr` (`Q:hw/vfio/region.c:294`).
- C9. Frames leave via TAP only, `IFF_TAP | IFF_NO_PI`, no vnet header (`D:ionic_eth_net.c:64,112,133`;
  same at pin `P:ionic_eth_net.c:63`), after the optional TX filter (`D:...:2061-2064`).
- C10. One-VM + addressed host TAP and two-TAPs-on-a-bridge recipes (`D:docs/ionic.rst:272-326`);
  `sanity-tests.yml` pings, `tcp-performance-tests.yml` runs iperf3, `tasks/tap.yml` builds the
  bridge; qemu-minimal `vfio-user-ernic-2vm` uses a tapsetup container, `-T tap0`, image tag
  `ernic.8f35f4a` (`docker-compose.yml:75,95-96,132,154-155`). Note the compose passes `-T` only if
  `tap0` appeared in time; otherwise TX is a sink (V5 applies there too).
- C11. develop vs pin: pin Q_IDENTIFY reads `cmd[3]` (`P:...:1292`) and gives TXQ and RXQ the same
  8-elem SG answer only when that byte matches (it is 0 = adminq for Linux); pin Q_INIT stores no
  `sg` flag (`P:...:1417-1426` vs `D:...:1748`); pin RX uses one buffer, `-EMSGSIZE` if longer
  (`P:...:1757-1777`); pin has no bus-master gate (no `bus_master` in `P:src/ernic_dma.c` or
  `P:src/ionic/ionic_eth_emu.c`). TX gather and feature handling are textually the same logic.
- C12. MTU: frame buffer 9600 (`D:ionic_eth_net.h:15`, `D:ionic_eth_emu_priv.h:535`), default
  `mtu_max = 9198` (`D:ionic_profiles.c:43`). With the current `len` convention the gathered length
  equals `len <= 9600`, so there is no overflow today.
- C13. Pins: `kernel_tag v7.2.15`, `ernic_source_pin 61b934a3...`, `ionic_kernel_pin v7.2.6`
  (`kernel-tools/ernic-pins.yml:5,8,11`).

### UNVERIFIABLE (from the trees available)

- U1. udmabuf readable: plausible (no vIOMMU, DMA address == GPA in the memfd region) but not tested.
- U2. ram_device BAR mapped "without an fd": `qemu_ram_alloc_from_ptr` blocks get `fd = -1`
  (`Q:system/physmem.c:2552`), consistent with the claim, but the call chain was not traced end to
  end, and the fleet's forked QEMU image was not inspected.
- U3. Everything about the v7.2.x driver/core (including whether W1's `sk_setup_caps()` behaviour
  and the `netmem_tx` enum match): master only.
- U4. Which server commit the fleet runs; vIOMMU behaviour.
