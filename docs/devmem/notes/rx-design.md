# devmem TCP RX on ionic via an emulator header-split mode: design study

Read-only study, 2026-11-18. Nothing was built, booted or run for this note; every claim is
from reading source. No code was changed in any tree.

Trees and citation prefixes:

- `K74:` v7.4-rc3. Core files are cited from `/var/tmp/ionic-devmem/linux` (its working tree
  has uncommitted devmem-TX edits, but only under `drivers/net/ethernet/pensando/ionic/` and
  `Documentation/.../ionic.rst`, so core line numbers are v7.4-rc3). ionic files are cited from
  a pristine `git archive HEAD` extract (`/tmp/rxstudy/k74`), so ionic line numbers are
  v7.4-rc3, not the edited working tree.
- `K7215:` `/var/tmp/ionic-devmem/linux-7.2.15`; `K726:` `/var/tmp/ionic-devmem/linux-7.2.6`.
- `E:` `~/Projects/rocm-ernic` at commit bd902345 (`git show HEAD:...`).
  The working tree (branch `feat/devmem-poc`) was being edited by a concurrent TX-offload
  step while this was written and its line numbers moved under me, so everything is cited
  against the commit. The uncommitted edits touch TX, LIF features and Q_IDENTIFY for TXQ;
  the RX functions cited here (`eth_rx_bufs()`, `eth_rx_deliver()`,
  `ionic_eth_emu_rx_steer()`, `ionic_eth_emu_poll_rx()`) read the same in both.
- `NC:` `K74:tools/testing/selftests/.../net/hw/ncdevmem.c`.

Labels: VERIFIED = read in source at the cited line; INFERRED = reasoned from verified facts;
UNKNOWN = cannot be answered from any tree here.

## 0. Decisive facts

1. VERIFIED. RX bind has six hard gates in the core, all of which stock ionic fails:
   ops-locked netdev, `queue_mgmt_ops` with all four alloc/free/start/stop callbacks,
   `hds_config == ENABLED`, `hds_thresh == 0`, no XDP program, and a page_pool created for
   that queue with `PP_FLAG_ALLOW_UNREADABLE_NETMEM` + `DMA_MAP` + `DMA_SYNC_DEV`.
2. VERIFIED. The core never checks that the device really splits. It checks configuration
   only. Whether payload lands in the dmabuf is entirely the driver's and device's doing.
3. VERIFIED. An unreadable RX skb that arrives `CHECKSUM_NONE` cannot be received:
   `skb_checksum()` hits `WARN_ON_ONCE(!skb_frags_readable(skb))` and returns 0
   (`K74:net/core/skbuff.c:3570`). ionic only ever reports `CHECKSUM_COMPLETE`, from
   `IONIC_RXQ_COMP_CSUM_F_CALC` + `comp->csum` (`K74:ionic_txrx.c:684-691`). The emulator leaves
   `csum_flags` zero (`E:src/ionic/ionic_eth_emu.c:2221-2226`) and does not grant
   `IONIC_ETH_HW_RX_CSUM`: bd902345 grants no LIF feature at all (:1142), and the working
   tree's uncommitted mask is TX_SG, TX_CSUM and three TSO bits, no RX bit
   (`ionic_eth_emu_priv.h`, `IONIC_EMU_ETH_HW_FEATURES`). **RX checksum in the
   emulator is therefore a prerequisite as hard as header split.** This was not in the brief.
4. VERIFIED. ionic's RX path puts the whole frame, headers included, into page frags
   (`napi_get_frags()` + `skb_add_rx_frag()` + `napi_gro_frags()`, `K74:ionic_txrx.c:194-221,
   724`). netmem.rst forbids mixing page and provider frags in one skb
   (`K74:Documentation/networking/netmem.rst:68-73`), so HDS mode needs a new skb shape:
   headers in the linear area, payload-only frags.
5. VERIFIED. `ionic_rxq_comp` is 16 bytes with no spare field
   (`K74:ionic_if.h:1150-1160`), so a header length cannot be reported in the existing
   completion. The existing precedent for extra per-completion data is the 2X completion used
   for hardware timestamps (`IONIC_Q_F_2X_CQ_DESC | IONIC_RXQ_F_HWSTAMP`,
   `K74:ionic_lif.c:1064, 1120`; `K74:ionic_txrx.c:733-738, 705-711`).
6. VERIFIED. The emulator already fills buffer 0 to its posted length and then the SG elements
   in order, with no minimum on buffer 0 (`E:src/ionic/ionic_eth_emu.c:2153-2190, 2208-2218`).
   Option B (driver-only, small buffer 0) therefore "works" on the emulator unmodified, but it
   is a fixed-prefix split, not a header split (section 4).
7. VERIFIED. The emulator offers multiple RX queues (profile `queues.eth`, default 4, max 64:
   `E:src/ionic/ionic_profile.c:119`, `ionic_profile.h:84`) and steers by its own FNV-1a
   flow hash modulo the enabled queues (`E:src/ionic/ionic_eth_emu.c:2307-2311`,
   `ionic_eth_steer.c`). It ignores the guest's RSS key and indirection table and keeps no
   state for `RX_FILTER_ADD` (`E:src/ionic/ionic_eth_emu.c:1867-1870`). `ethtool -X` succeeds and
   does nothing.
8. VERIFIED. ionic has no n-tuple support: no `get_rxnfc`/`set_rxnfc`, no RSS contexts
   (`K74:ionic_ethtool.c:1146-1181`); `ionic_if.h` filter matches are VLAN, MAC, MAC_VLAN and
   `STEER_PKTCLASS` only (`K74:ionic_if.h:2059-2062`). But `ionic_rx_filter_add_cmd` already
   carries `qtype` and `qid` (`K74:ionic_if.h:2082-2101`), used to steer PTP to the hwstamp
   queue (`K74:ionic_lif.c:1183-1217`), so a flow-steer match type fits the existing command.
9. VERIFIED. v7.2.15 differs from v7.4-rc3 in nothing that matters for RX:
   `net/core/netdev_rx_queue.c` and `net/core/page_pool.c` are byte-identical, ionic
   `ionic_txrx.c` is byte-identical, `ionic_lif.c` differs only in allocation macros and the
   `RDMA_TIMESTAMP` feature bit. v7.2.6 vs v7.2.15 `ionic_txrx.c` differs only in three TX
   hunks. `IONIC_ETH_HW_RDMA_TIMESTAMP = BIT(21)` exists only at v7.4
   (`K74:ionic_if.h:1213`; last bit at `K7215:ionic_if.h:1183` is BIT(20)).

## 1. Core requirements for devmem RX bind

All VERIFIED at v7.4-rc3 unless marked. `net/core/netdev_rx_queue.c` is identical at v7.2.15
and v7.2.6, so the gate list holds there too.

### 1.1 `netdev_nl_bind_rx_doit()` (`K74:net/core/netdev-genl.c:1031-1154`)

- Needs `NETDEV_A_DEV_IFINDEX`, `NETDEV_A_DMABUF_FD`, `NETDEV_A_DMABUF_QUEUES` (:1044-1047).
  Optional `rx-page-size` must be a power of two (:1052-1062; v7.4 only, the attribute is
  absent at v7.2.15 per `selftest-ncdevmem.md`).
- `netif_device_present()` else `-ENODEV` (:1086). ionic calls `netif_device_detach()` around
  every reconfigure (`K74:ionic_lif.c:1772`), which matters for a "full reconfigure" queue-op
  stub: the device is detached while the core is mid-bind (harmless, the check is before).
- **`netdev_need_ops_lock()` else `-EOPNOTSUPP`** (:1088-1089). True iff
  `dev->request_ops_lock || dev->queue_mgmt_ops` (or net_shaper ops)
  (`K74:include/net/netdev_lock.h:28-37`). ionic sets neither (grep of
  `K74:drivers/net/ethernet/pensando/` for `netdev_lock|ops_lock|queue_mgmt`: no hits).
- DMA device: `netdev_nl_get_dma_dev()` (:1007-1029) calls `netdev_queue_get_dma_dev()`, which
  uses `ndo_queue_get_dma_dev` if present, else `dev->dev.parent`, and requires a `dma_mask`
  (`K74:net/core/netdev_queues.c:10-21`). For ionic the parent is the PCI device, so the
  optional callback is not needed.
- `net_devmem_bind_dmabuf(..., DMA_FROM_DEVICE, ...)` (:1113), then per queue
  `net_devmem_bind_dmabuf_to_queue()` (:1121-1126).

### 1.2 `net_devmem_bind_dmabuf_to_queue()` -> `netif_mp_open_rxq()`

`K74:net/core/devmem.c:152-183` builds `pp_memory_provider_params{mp_priv = binding,
mp_ops = &dmabuf_devmem_ops}` and calls `netif_mp_open_rxq()`
(`K74:net/core/netdev_rx_queue.c:245-276`), which runs `__netif_mp_open_rxq()` (:187-243):

| check | line | error | ionic today |
| --- | --- | --- | --- |
| `netdev_need_ops_lock()` | 251 | `-EOPNOTSUPP` | fails |
| `rxq_idx < real_num_rx_queues` | 254 | `-ERANGE` | ok |
| `dev->queue_mgmt_ops` | 196 | `-EOPNOTSUPP` | fails |
| `dev->cfg->hds_config == ETHTOOL_TCP_DATA_SPLIT_ENABLED` | 199 | `-EINVAL` "tcp-data-split is disabled" | fails |
| `dev->cfg->hds_thresh == 0` | 203 | `-EINVAL` | ok (stays 0 if the driver never claims `ETHTOOL_RING_USE_HDS_THRS`) |
| `dev_xdp_prog_count(dev) == 0` | 207 | `-EEXIST` | ok unless XDP attached |
| `rx_page_size` needs `QCFG_RX_PAGE_SIZE` | 211 | `-EOPNOTSUPP` | n/a for PAGE_SIZE niovs |
| queue not already provider-bound | 217 | `-EEXIST` | ok |
| queue not an AF_XDP pool | 222 | `-EBUSY` | ok (ionic has no XSK) |

Then `rxq->mp_params = *p` (:229), `netdev_queue_config_validate()` (:230), and
`netdev_rx_queue_reconfig()` (:98-176):

1. All of `ndo_queue_stop`, `ndo_queue_mem_free`, `ndo_queue_mem_alloc`, `ndo_queue_start`
   must exist, else `-EOPNOTSUPP` (:108-110).
2. `ndo_queue_mem_alloc(dev, qcfg_new, new_mem, idx)` (:124). This is where the driver must
   create the new page_pool, because the next step is
3. `page_pool_check_memory_provider()` (:128): there must be a page_pool on `dev->page_pools`
   with `mp_priv == binding` and `slow.queue_idx == idx`, else `-ENODATA`
   (`K74:net/core/page_pool_user.c:401-423`).
4. If the netdev is running: `ndo_queue_stop(dev, old_mem, idx)` then
   `ndo_queue_start(dev, qcfg_new, new_mem, idx)` (:132-139); on start failure the old memory
   is restarted (:159). If not running, the memories are just swapped (:141).
5. `ndo_queue_mem_free(dev, old_mem)` (:144).

Unbind runs the same reconfigure with `mp_params` cleared (:278-304).
`netdev_rx_queue_restart()` (:178-185) is the same thing with identical old and new config.

`struct netdev_queue_mgmt_ops` (`K74:include/net/netdev_queues.h:206-233`):
`ndo_queue_mem_size`, the four callbacks, optional `ndo_default_qcfg`, `ndo_validate_qcfg`,
`ndo_queue_get_dma_dev`, `ndo_queue_create`, and `supported_params` (`QCFG_RX_PAGE_SIZE`,
:156-159). `mem_alloc`/`mem_free` may be called with the interface down (:202-204); bnxt
returns an error from `mem_alloc` in that case (ca2456e07395). At v7.2.15 the struct has the
same callbacks; `netdev_queue_config` lacks the three ring-size fields v7.4 added.

### 1.3 page_pool and the memory provider

- `page_pool_init()`: with `PP_FLAG_ALLOW_UNREADABLE_NETMEM` the pool copies
  `mp_priv`/`mp_ops` from `rxq->mp_params` for `slow.queue_idx`, under
  `netdev_assert_locked()` (`K74:net/core/page_pool.c:275-281`). With a provider the pool must
  have `dma_map` and `dma_sync` or creation fails `-EOPNOTSUPP` (:283-287), and `mp_ops->init`
  runs (:294).
- `mp_dmabuf_devmem_init()` forces `dma_sync = false`, requires
  `pool->p.order == binding->niov_shift - PAGE_SHIFT` (order 0 for 4K niovs), takes a binding
  ref (`K74:net/core/devmem.c:468-486`). `alloc_netmems` hands out net_iovs (:488-510);
  `release_netmem` returns them to the binding (:519-534).
- ionic's pool today: `PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV`, order 0, `.netdev` set,
  no `.queue_idx`, no unreadable flag (`K74:ionic_lif.c:591-601`). Two of the three flags are
  already right.
- netmem.rst "Driver RX Requirements" (`K74:Documentation/networking/netmem.rst:75-131`):
  page_pool; tcp-data-split; netmem APIs for payload (`page_pool_alloc_netmem`,
  `page_pool_get_dma_addr_netmem`, `page_pool_put_netmem`); the three PP flags, with
  `ALLOW_UNREADABLE_NETMEM` "iff tcp-data-split is enabled"; never assume readable
  (`netmem_address()` may be NULL); `page_pool_dma_sync_netmem_for_cpu()` instead of
  `dma_sync_*`; no driver-private page recycling.

### 1.4 ethtool tcp-data-split plumbing

- Capability: `ethtool_ops->supported_ring_params & ETHTOOL_RING_USE_TCP_DATA_SPLIT`
  (`K74:include/linux/ethtool.h:118`), else setting the attribute is rejected
  (`K74:net/ethtool/rings.c:165-168`). `ETHTOOL_RING_USE_HDS_THRS` (:119) gates `hds-thresh`
  (:173-174). ionic has only `TX_PUSH | RX_PUSH` (`K74:ionic_ethtool.c:1150-1151`).
- GET reports `dev->cfg->hds_config` / `hds_thresh` (`K74:net/ethtool/rings.c:44-45`).
- SET: rejects ENABLED with a single-buffer XDP program (:253-259); rejects disabling HDS or a
  non-zero threshold while any queue has a provider (:261-272); stores the request in
  `dev->cfg_pending` (:301-302) and calls the driver's `set_ringparam` (:309), which sees
  `kernel_ringparam.tcp_data_split`.
- XDP exclusion both ways: no non-frags XDP while HDS is enabled
  (`K74:net/core/dev.c:10551, 10629`), no XDP at all while a provider is bound (:10559, 10635).
- Channels: `ethtool_check_max_channel()` refuses to shrink below a provider-bound queue
  (`K74:net/ethtool/common.c:798-823`, `dev_get_min_mp_channel_count()`
  `K74:net/core/dev.c:11048`). ncdevmem tests exactly this (NC:1162-1166).

### 1.5 Keeping other flows off the queue

Not enforced by the core; it is the operator's job (`K74:Documentation/networking/devmem.rst:
79-98`: "Header split, flow steering, & RSS are required features"). ncdevmem does
`ethtool -X IF equal <start_queue>` (NC:561-564) then `ethtool -N IF flow-type tcp{4,6} ...
queue <start_queue>`, 5-tuple first, 3-tuple fallback (NC:662-696). `-n` skips header split,
RSS and flow steering setup (NC:1463-1465, 872-894); `-t N -q M` picks the queues, otherwise
it needs >= 2 RX queues and binds the last (NC:1518-1534).

What happens if a stray flow does land there: a normal `recvmsg()` on an unreadable skb gets
`-EFAULT` (`K74:net/ipv4/tcp.c:2860-2867`); anything that needs payload in the kernel
(`skb_copy_bits`, `__pskb_pull_tail`, `skb_checksum`, `skb_copy`) fails or warns
(`K74:net/core/skbuff.c:2906, 3062, 3570, 2189`). Packets that fit entirely in the header
buffer are ordinary readable skbs, so ARP, neighbour discovery, small ICMP and bare ACKs are unaffected. That
is what makes a single-queue PoC viable (section 5.3).

### 1.6 What the driver must never do with an unreadable RX skb

- Copybreak or any copy out of payload buffers; gve disables rx_copybreak for unreadable
  payload (62d7f40503bc). ionic's `ionic_rx_copybreak()` copies from buffer 0 only
  (`K74:ionic_txrx.c:260`), so it is safe only if buffer 0 is always a host page.
- Put headers in a frag and let GRO pull them: `napi_gro_frags()` pulls the Ethernet header
  out of frag 0. Headers must be in the linear area.
- `page_address()`, `dma_sync_*`, `kmap` on payload; XDP on the queue.
- Software checksum: deliver `CHECKSUM_COMPLETE` or `CHECKSUM_UNNECESSARY` (fact 0.3).
- Mark the skb for page_pool recycling: `skb_mark_for_recycle()` is required so the net_iov
  goes back through `napi_pp_put_page()` (`K74:net/core/skbuff.c:1040-1080`). ionic already
  does this (`K74:ionic_txrx.c:201, 252`).
- Hold driver-private references to payload buffers across packets. ionic keeps unused
  buffers attached to the descriptor slot for reuse (`K74:ionic_txrx.c:831, 852-867`); that is
  page_pool-owned memory, not private recycling, and is fine with netmem (INFERRED).

GRO and the socket side need no driver work: `skb->unreadable` is set automatically when a
net_iov frag is added (`K74:include/linux/skbuff.h:2604-2612`);
`tcp_recvmsg_dmabuf()` copies any linear payload to the user buffer and reports it as
`SCM_DEVMEM_LINEAR`, and reports each net_iov frag as `SCM_DEVMEM_DMABUF` with a token
(`K74:net/ipv4/tcp.c:2510-2655`); tokens are returned with `SO_DEVMEM_DONTNEED`
(`K74:net/core/sock.c:1300`).

## 2. ionic's RX path today

VERIFIED at v7.4-rc3; identical at v7.2.15 for everything below.

### 2.1 Buffers

- page_pool since ac8813c0ab7d "ionic: convert Rx queue buffers to use page_pool"
  (v6.12-rc1). One pool per RX qcq, created in `ionic_qcq_alloc()`
  (`K74:ionic_lif.c:590-613`), destroyed in `ionic_qcq_free()` (:453).
- `struct ionic_buf_info { struct page *page; dma_addr_t dma_addr; u32 page_offset; u32 len; }`
  (`K74:ionic_dev.h:206-211`), `struct ionic_rx_desc_info { nbufs; bufs[1 + 8]; }` (:225-228).
  The pending TX series already gives TX its own struct
  (`/var/tmp/ionic-devmem/patches/mainline/0001-*`), which frees `ionic_buf_info` to become
  netmem-based for RX.
- `ionic_rx_fill()` (`K74:ionic_txrx.c:776-894`): `len = mtu + VLAN_ETH_HLEN` (:802). Buffer 0
  is `min(len, IONIC_PAGE_SIZE)` (:815), or with XDP the linear-MTU size plus
  `XDP_PACKET_HEADROOM` (:810-812). It is allocated with the frag API
  `page_pool_alloc(pool, &offset, &len, GFP_ATOMIC)` (:832). Remaining length goes into up to
  `q->max_sg_elems` SG elements of up to a page each (:849-874), a zero element terminates the
  list (:877-878), and the opcode is `SG` if more than one buffer, else `SIMPLE` (:880-881).
  `IONIC_PAGE_SIZE = MIN(PAGE_SIZE, 65535)` (`K74:ionic_dev.h:198-199`).
- So today buffer 0 is never smaller than `min(mtu + 18, 4096)` (about 1518 at the default
  MTU). The Linux driver has never posted a small buffer 0 to real firmware.

### 2.2 Descriptor, SG list, completion (`K74:ionic_if.h:1044-1160`)

```
struct ionic_rxq_desc    { u8 opcode; u8 rsvd[5]; __le16 len; __le64 addr; }        16 bytes
struct ionic_rxq_sg_elem { __le64 addr; __le16 len; __le16 rsvd[3]; }               16 bytes
struct ionic_rxq_sg_desc { elems[8] }      IONIC_RX_MAX_SG_ELEMS 8, STRIDE 8       128 bytes
struct ionic_rxq_comp    { u8 status; u8 num_sg_elems; __le16 comp_index;
                           __le32 rss_hash; __le16 csum; __le16 vlan_tci;
                           __le16 len; u8 csum_flags; u8 pkt_type_color; }          16 bytes
```

Only `OPCODE_SIMPLE` is documented ("Receive full packet into data buffer starting at
@addr"). The SG fill rule is not written down anywhere in `ionic_if.h`; the driver encodes it
in `ionic_rx_build_skb()`: frag 0 is `min(len, first buffer)`, each SG frag is
`min(remaining, buf_info->len)` for `comp->num_sg_elems` elements
(`K74:ionic_txrx.c:203-221`). That is "fill each buffer completely before the next".
A status of 2 means the packet did not fit (`K74:ionic_txrx.c:628-638`).

### 2.3 skb construction

`ionic_rx_clean()` (`K74:ionic_txrx.c:612-725`): XDP first if a program is attached (:644);
`len <= lif->rx_copybreak` (default 256, `K74:ionic_lif.h:23`) takes `ionic_rx_copybreak()`
(`napi_alloc_skb` + copy from buffer 0, :230-272) and `napi_gro_receive()`; otherwise
`ionic_rx_build_skb()` and `napi_gro_frags()` (:721-724). RSS hash from `comp->rss_hash` by
packet type (:667-682). Checksum: `CHECKSUM_COMPLETE` with `skb->csum = le16_to_cpu(comp->csum)`
only when `NETIF_F_RXCSUM` and `CSUM_F_CALC` (:684-691); the `_OK`/`_BAD` flags feed a counter
only (:693-696). `NETIF_F_RXCSUM` comes from `IONIC_ETH_HW_RX_CSUM`
(`K74:ionic_lif.c:1467-1468, 1586`). 2X completions: the standard 16 bytes are the trailing
half (:733-738) and the timestamp sits 8 bytes before them (:705-711,
`IONIC_HWSTAMP_CQ_NEGOFFSET = 8`, `K74:ionic_if.h:452`).

### 2.4 XDP

`ionic_run_xdp()` (`K74:ionic_txrx.c:470-610`) reads buffer 0 through `page_address()`.
`IONIC_QINIT_F_SG` is dropped for a non-frags program (`K74:ionic_lif.c:939-941`). XDP is
refused with split interrupts (:2769-2774). The pool becomes `DMA_BIDIRECTIONAL` (:603-604).

### 2.5 Queue lifecycle

- Per-queue commands exist: `Q_INIT` (`ionic_lif_rxq_init()`, `K74:ionic_lif.c:898-985`) and
  `Q_CONTROL` enable/disable by (type, index) (`ionic_qcq_enable()`/`ionic_qcq_disable()`,
  :292-383). There is no per-queue deinit command; `ionic_lif_qcq_deinit()` is host-side only
  (:385-403). After disabling all queues the driver also sends `LIF_SETATTR(STATE, QUIESCE)`
  (:1974-1990, 2013).
- No path today stops and restarts one RX queue of a running LIF. The nearest precedents:
  (a) `ionic_lif_create_hwstamp_rxq()` does `Q_INIT` + fill + enable of an additional RX queue
  while the others run (:1087-1098); (b) `ionic_reconfigure_queues()` (:2946-3195) allocates
  new qcqs with new page_pools while traffic runs (:2984-3050), then stops and deinits
  everything (`ionic_stop_queues_reconfig()`, :1769-1775), swaps ring state with
  `ionic_swap_queues()` (:2902-2944), and re-`Q_INIT`s every queue without a LIF reset
  (`ionic_start_queues_reconfig()` -> `ionic_txrx_init()`, :1777-1794, 2170-2201). So
  re-`Q_INIT` of a previously used index is established; doing it for one index while the LIF
  is not quiesced is not.
- TX and RX of the same index share one interrupt and one NAPI (`ionic_txrx_napi`) unless
  `IONIC_LIF_F_SPLIT_INTR` (:972-975, 2103-2104). Stopping RX queue i therefore also stalls TX
  completions for TX queue i; a per-queue restart must stop that TX subqueue too.
- `ionic_reconfigure_queues()` already rebuilds RX queues when `qparam->rxq_features` changes
  (:2969-2972, 3024-3025, 3047), and sizes completions 2X when `IONIC_Q_F_2X_CQ_DESC` is set.
  `lif->rxq_features` goes to the device in `Q_INIT.features` (:918).
- Queues are started from a deferred link-status work item under `lif->queue_lock`, not from
  `ndo_open` when the link is down (:162-175, 2311-2316). Other non-ndo paths that tear down
  or start queues: tx-timeout work (:1849-1870), FW down/up (:3389-3432),
  `ionic_bus_pci.c:457-463`.

### 2.6 Locking (why ops-locking is the expensive part)

`napi_enable()`, `napi_disable()`, `netif_napi_add()` and `netif_napi_del()` take
`netdev_lock()` themselves (`K74:net/core/dev.c:7908-7948`,
`K74:include/linux/netdevice.h:2932-2944, 3007-3012`). Once a netdev is ops-locked, ndos and
ethtool ops run with that lock held (`K74:Documentation/networking/netdevices.rst:343-405`).
ionic calls the unlocked variants from `ndo_open`/`ndo_stop`/ethtool paths
(`K74:ionic_lif.c:325, 367, 399, 891, 973-978`). Setting `queue_mgmt_ops` or
`request_ops_lock` without converting them deadlocks on the first `ip link set up` (INFERRED
from the two facts; not run). The work-item paths in 2.5 need the opposite fix: take
`netdev_lock()` before `queue_lock`. bnxt did this as a separate series before its queue ops
were usable (004b5008016a, eaca6e5dc6ba).

### 2.7 ethtool, steering

Ring params: sizes and `tx_push`/`rx_push` only (`K74:ionic_ethtool.c:686-755`). RSS:
`get/set_rxfh` with key and indirection table (:874-920); `ionic_lif_rss_config()` sends
`LIF_SETATTR(RSS)` with the table's DMA address (`K74:ionic_lif.c:1910-1941`); the table
defaults to `ethtool_rxfh_indir_default()` (:1958) and RSS init runs only with
`NETIF_F_RXHASH` (:2187-2188). No n-tuple, no RSS contexts (fact 0.8). So on real hardware
today the only steering is by hash: `ethtool -X equal N` can keep flows off a queue, nothing
can put a chosen flow onto it.

## 3. How reference drivers did RX

Hashes VERIFIED with `git show --shortstat` in `/var/tmp/ionic-devmem/linux`; "first in"
from `git describe --contains`.

| driver | HDS | queue API | netmem / unreadable | notes |
| --- | --- | --- | --- | --- |
| bnxt | pre-existing hardware HDS; ethtool knob 87c8f8496a05 (v6.14, +28/-3), hds-thresh 6b43673a25c3 (+10/-2); head frags to page_pool 7ed816be35ab (v6.13, +51/-48) | 2d694c27d32e (v6.11, +275, one file), later fixes for TPA, RSS table, xdp_rxq_info, leaks (bd649c5cc958, 5ac066b7b062, b537633ce57b, 87dd2850835d, d1000fd7995e) | cd1fafe7da1f (v6.16, +133/-82) "requires only converting the page API to netmem API"; ops lock eaca6e5dc6ba (v6.15) | first devmem RX driver; separate head pool when the payload pool is unreadable (`bnxt.c:3975`) |
| gve | device option 0b43cf527d1d (+70/-16) and data path 5e37d8254e7f (+169/-8), v6.9 | c93462b914db (v6.10, +189/-24) | aa3e360701c3 (v6.15, +47/-22), 62d7f40503bc (v6.18, +35/-5) | completion carries `header_len:10`, `split_header:1`, `header_buffer_overflow:1` (`gve_desc_dqo.h:228-235`); header copied into a `napi` skb (`gve_rx_dqo.c:872-886`); copybreak off for unreadable |
| mlx5e | SHAMPO (pre-existing); ethtool 46bcce5dfd33 (+36/-5) | b2588ea40ec9 (+98) "single channel swap" | d1668f119943 (+63/-44), db3010bb5a01 (+8/-1); dma dev op f1debf1a2ef4 (v6.18, +24) | all v6.17 except the last |
| fbnic | 2b30fc01a6c7 (+43/-8) | da43127a8edc (+174/-2) "doesn't shut down the entire device just to restart a single queue" | 33478dca2b23 (+55/-44), 426e13db369c (+40/-27), 8a11010fdd96 (+21/-10) | all v6.18; separate header and payload pools; `FBNIC_HDR_BYTES_MIN` 256 |
| netdevsim | f394d07b192b (v6.14, +23/-1): a software HDS knob | has `nsim_queue_mgmt_ops` (`netdevsim/netdev.c:863-867`) | none: its pool has no `DMA_MAP`/unreadable flag (:425-436) | precedent for a software HDS knob and queue ops, **not** for devmem RX |

Shape common to all four real drivers: (1) hardware reports where the header ends, (2) header
goes to the skb linear area from a host buffer, (3) payload pool is a separate netmem pool
flagged unreadable only when HDS is on, (4) queue ops restart one queue. Sizes for a driver
that already had HDS and page_pool: roughly 150 to 300 lines for queue ops plus 50 to 150 for
netmem. ionic has neither HDS nor ops locking, so it starts further back than any of them.

## 4. Device-side options

### 4.1 Option B: no device change, small buffer 0

The driver posts buffer 0 of N bytes from a host-page pool and page-sized SG elements from
the payload pool. By the fill rule the first N bytes of every frame land in buffer 0.

- Emulator: works unmodified (fact 0.6).
- Core: passes, because the core checks configuration only (fact 0.2). But the driver would
  be advertising tcp-data-split while delivering a fixed-prefix split. With `hds_thresh == 0`
  the documented meaning is that no payload shares the header buffer. Not acceptable upstream.
- TCP devmem semantics: with N = 256 and a 66-byte IPv4+TCP+timestamps header, 190 payload
  bytes of every full segment arrive in the linear area. `tcp_recvmsg_dmabuf()` copies them to
  the user buffer and emits `SCM_DEVMEM_LINEAR` (`K74:net/ipv4/tcp.c:2530-2562`), so the byte
  stream is intact but split across two channels per segment. Stock ncdevmem ignores linear
  bytes ("TODO", NC:991-1005) while `validate_buffer()` keeps a running pattern position
  (NC:278-305), so `-v` validation reports mismatches, and `-L` fails outright. A custom
  receiver would be needed.
- Tuning N to the exact header length does not rescue it: a SYN carries more options than a
  data segment, so its tail would spill into unreadable memory and the handshake would fail
  (INFERRED).
- Real firmware: UNKNOWN whether a buffer 0 smaller than the frame headers, or smaller than
  some minimum, is accepted; Linux has never posted one (2.1).

Verdict: useful only as a way to exercise the driver's new skb shape before the emulator
feature exists. Not a deliverable.

### 4.2 Option A: negotiated header split (recommended)

**This is a PoC interface defined by this project so the emulator can stand in for future
firmware. It is not a Pensando/AMD firmware commitment, and the bit numbers below must not be
upstreamed without the firmware team assigning them.**

Design choices, each following an existing ionic precedent:

- Per-queue feature in `Q_IDENTIFY.features` / `Q_INIT.features`, next to
  `IONIC_RXQ_F_HWSTAMP`, because it changes how a queue's descriptors are interpreted and
  the driver already carries `rxq_features` through reconfigure (2.5). A driver that does not
  know the bit never sets it in `Q_INIT`, so advertising it is invisible to stock guests.
- Header length in the leading half of a 2X completion, as the timestamp is.
- No new descriptor opcode and no new `Q_INIT` field: the header buffer capacity is the
  descriptor's own `len`.

```c
/* ionic_if.h additions. EXPERIMENTAL: emulator-only, not a firmware interface. */

enum ionic_rxq_feature {
	IONIC_RXQ_F_HWSTAMP	= BIT_ULL(16),
	IONIC_RXQ_F_HDS		= BIT_ULL(17),	/* header/data split; needs IONIC_Q_F_2X_CQ_DESC
						 * and IONIC_QINIT_F_SG */
};

/**
 * struct ionic_rxq_comp_ext - leading 16 bytes of a 2X Rx completion
 * @hds_hdr_len:  Bytes written to the descriptor's own buffer (@addr).
 *                Valid when the queue has IONIC_RXQ_F_HDS.
 * @hds_flags:    See IONIC_RXQ_COMP_HDS_F_*
 * @rsvd:         reserved, zero
 * @hwstamp:      Timestamp, valid when the queue has IONIC_RXQ_F_HWSTAMP
 *                (existing field, IONIC_HWSTAMP_CQ_NEGOFFSET = 8), else zero
 */
struct ionic_rxq_comp_ext {		/* offset in the 32-byte completion */
	__le16 hds_hdr_len;		/*  0 */
	u8     hds_flags;		/*  2 */
	u8     rsvd[5];			/*  3 */
	__le64 hwstamp;			/*  8 */
};					/* 16: struct ionic_rxq_comp follows */

#define IONIC_HDS_CQ_NEGOFFSET		16
#define IONIC_RXQ_COMP_HDS_F_SPLIT	0x01	/* buffer 0 holds exactly the L2..L4 headers */
#define IONIC_RXQ_COMP_HDS_F_HBO	0x02	/* headers longer than buffer 0; not split */
```

Device behaviour on a queue initialised with `IONIC_RXQ_F_HDS`, for an
`IONIC_RXQ_DESC_OPCODE_SG` descriptor:

1. Parse Ethernet, up to two VLAN tags, IPv4 (any IHL) or IPv6 (fixed header only), TCP
   (data offset honoured). `hdr_len` = offset of the first TCP payload byte. UDP may be split
   the same way (8-byte header); devmem does not need it.
2. Split when the parse succeeds, the packet is not an IPv4 fragment (MF clear, offset zero),
   the L4 header is wholly inside the frame, and `hdr_len <= desc.len`. Bytes `[0, hdr_len)`
   go to `desc.addr`; bytes `[hdr_len, len)` go to SG elements 0, 1, ... each filled to its
   `len` before the next. `comp.num_sg_elems` = elements used (0 for a pure ACK),
   `comp.len` = whole frame length as today, `ext.hds_hdr_len = hdr_len`,
   `ext.hds_flags = SPLIT`.
3. Otherwise (non-IP, other L4, IPv6 extension headers, fragments, runts, truncated headers):
   fill exactly as a non-HDS SG queue does, buffer 0 first. `ext.hds_hdr_len =
   min(len, desc.len)`, `SPLIT` clear; set `HBO` when the parse succeeded but
   `hdr_len > desc.len`.
4. A frame the posted buffers cannot hold is dropped as today.
5. `OPCODE_SIMPLE` descriptors on an HDS queue are filled as today with
   `hds_hdr_len = len`.
6. `comp.csum` covers the L2 payload regardless of where the bytes were placed.

Because `hds_hdr_len` is always "bytes in buffer 0", the driver has one rule: copy
`hds_hdr_len` bytes from buffer 0 into the linear area, then attach `num_sg_elems` payload
frags. In case 3 on a devmem queue the frags may hold unparsed bytes of a non-TCP packet; the
stack then fails its pull and drops it, which is the same outcome as on gve.

Flow steering extension (needed for the stock selftest, not for first light):

```c
	IONIC_ETH_HW_RX_FLOW_STEER	= BIT(22),	/* LIF feature <-> NETIF_F_NTUPLE */

	IONIC_RX_FILTER_STEER_FLOW	= 0x11,		/* enum ionic_rx_filter_match_type */

	/* new member of the ionic_rx_filter_add_cmd union (54 bytes available);
	 * cmd->qtype / cmd->qid name the target queue, as for STEER_PKTCLASS */
	struct {
		u8     ip_ver;		/* cmd offset 10: 4 or 6 */
		u8     ip_proto;	/* 11: 6 (TCP) or 17 (UDP) */
		__le16 fields;		/* 12: IONIC_RX_FLOW_F_* naming the fields to match */
		__le16 sport;		/* 14 */
		__le16 dport;		/* 16 */
		u8     src_ip[16];	/* 18: network order; IPv4 in the first 4 bytes */
		u8     dst_ip[16];	/* 34 */
	} flow;				/* ends at 50 of 64 */
#define IONIC_RX_FLOW_F_SRC_IP	0x01
#define IONIC_RX_FLOW_F_DST_IP	0x02
#define IONIC_RX_FLOW_F_SPORT	0x04
#define IONIC_RX_FLOW_F_DPORT	0x08
```

A matching flow filter takes precedence over RSS. BIT(22) is the next free LIF feature bit at
v7.4-rc3; BIT(21) is free at v7.2.15 but taken at v7.4 and must not be used. RSS itself needs
no interface change, only an emulator that honours `LIF_SETATTR(RSS)`.

### 4.3 What each option needs in the emulator

| item | Option B | Option A |
| --- | --- | --- |
| RX checksum: compute the ones-complement sum of the L2 payload into `comp.csum`, set `CSUM_F_CALC` (+ `_OK` flags), grant `IONIC_ETH_HW_RX_CSUM` | required | required |
| `Q_IDENTIFY(RXQ).features` (now 0, `E:ionic_eth_emu.c:1624-1631`) advertises `2X_CQ_DESC | RXQ_F_HDS` | no | yes |
| `Q_INIT` reads `features` (le64 at offset 56; not read today, `E:...:1694-1712`), validates HDS needs 2X + SG, records per-queue completion size | no | yes |
| per-queue completion stride: `ETH_COMP_SIZE` is a constant 16 (`E:ionic_eth_emu_priv.h:359`), used for the CQ address (`E:ionic_eth_emu.c:2234-2238`) | no | yes |
| header parser: `ionic_eth_steer.c` already walks VLAN/IPv4/IPv6 to the L4 offset; factor it out and add the TCP data offset | no | yes |
| split placement in `eth_rx_bufs()`/`eth_rx_deliver()` | no | yes |
| per-queue stop/start: `Q_CONTROL` already gates steering per queue and `Q_INIT` re-initialises one queue (`E:...:1803-1809, 1742-1752`) | ok | ok |
| steering (see below) | optional | optional |

Rough emulator size for A: 250 to 400 lines of C plus unit tests with golden frames, a
changelog entry and docs; the checksum item is about 50 lines. An experimental profile key to
gate the HDS advertisement is advisable.

Checksum byte order is a trap: the driver does `skb->csum = le16_to_cpu(comp->csum)`
(`K74:ionic_txrx.c:687`). On a little-endian guest that equals the kernel's native sum when
the device stores the conventional big-endian-word sum high byte first (INFERRED; must be
tested, a wrong value shows up as "hw csum failure" from `netdev_rx_csum_fault()`).

### 4.4 Steering the emulator can offer

- Today: hash over enabled queues only; not controllable from the guest (fact 0.7).
- Zero-change: run the receiver with one channel (`ethtool -L IF combined 1`). Every frame
  reaches queue 0, and everything except the TCP payload stays readable (1.5).
- Small change: honour the RSS indirection table (read the 128-entry table from the DMA
  address in `LIF_SETATTR(RSS)`, index it with the existing hash), and grant
  `IONIC_ETH_HW_RX_HASH` so the driver programs it at init. `ethtool -X equal N` then works.
- Full: the `STEER_FLOW` filter above with a small table consulted before RSS, so
  `ethtool -N ... queue N` works and stock ncdevmem runs without `-n`.

## 5. Driver work list

Estimates are for v7.4-rc3 mainline, lines changed in `drivers/net/ethernet/pensando/ionic/`.
The RX hunks apply unchanged to the v7.2.6-based DKMS sources (fact 0.9), on top of the TX
series.

| # | item | size | risk | stub for PoC? |
| --- | --- | --- | --- | --- |
| 1 | `ionic_if.h`: feature bit, completion extension | 40 | low | no |
| 2 | netmem conversion of RX buffers: `ionic_buf_info.page` -> `netmem_ref`; `page_pool_alloc_netmem`, `page_pool_get_dma_addr_netmem`, `page_pool_put_full_netmem`, `page_pool_dma_sync_netmem_for_cpu`, `skb_add_rx_frag_netmem`; XDP keeps pages | 80-120 | low-medium | no |
| 3 | HDS datapath: header page_pool per RX queue; `ionic_rx_fill()` HDS layout (buffer 0 = 256 bytes from the header pool, SG elements = pages from the payload pool); `ionic_rx_clean()` HDS branch (`napi_alloc_skb` + copy `hds_hdr_len` + frags + `napi_gro_receive`); read the extension; counters | 200-250 | medium | no |
| 4 | ethtool: `ETHTOOL_RING_USE_TCP_DATA_SPLIT`, get/set through `rxq_features` and `ionic_reconfigure_queues()`; offer only if `qtype_info[RXQ].features` has HDS; refuse HDS with XDP attached and XDP with HDS on | 60 | low | no |
| 5 | ops locking: `_locked` NAPI calls in ndo/ethtool paths, `netdev_lock()` in link-check, tx-timeout, FW down/up and PCI reset paths; lock order netdev -> queue_lock | 100-200 | **high** | partly: FW-reset and PCI-reset paths can be left unconverted in a PoC that never triggers them; open/stop/ethtool/link-check cannot |
| 6 | `queue_mgmt_ops` | 120 (stub) / 300 (real) | medium-high | yes: `mem_alloc` builds a shadow RX qcq as `ionic_reconfigure_queues()` does; `stop` stops and deinits all queues; `start` swaps and re-inits all; `mem_free` frees the shadow. Real version stops only RX i and TX i |
| 7 | pool flags: `.queue_idx`, `PP_FLAG_ALLOW_UNREADABLE_NETMEM` on the payload pool iff HDS is on | 10 | low | no |
| 8 | flow steering: `get/set_rxnfc`, `NETIF_F_NTUPLE`, `STEER_FLOW` filters with replay | 300 | medium | yes: single queue, or emulator RSS |
| 9 | docs, queue stats, `ionic.rst` | 50 | low | yes |

Cannot be stubbed: 1-4, 7, the core of 5, some form of 6, and emulator RX checksum.

Things that fall out for free: a binding survives ifdown/ifup because `ionic_open()` recreates
the pools and `page_pool_init()` re-reads `rxq->mp_params`; `ndo_queue_get_dma_dev` is not
needed; hds-thresh is not needed.

### 5.1 Proposed patch split (driver)

1. ionic: use the locked NAPI helpers and take the netdev instance lock (prep, no functional
   change; `request_ops_lock = true`).
2. ionic: track RX buffers as netmem.
3. ionic: add the header/data split interface definitions.
4. ionic: split headers into the skb linear area when the queue has HDS.
5. ionic: support the tcp-data-split ring parameter.
6. ionic: implement netdev queue management ops.
7. ionic: allow unreadable netmem on the payload pool.
8. (later) ionic: add n-tuple flow steering.

Patches 1 and 2 are worthwhile and reviewable without any firmware change. 3-7 depend on the
invented interface.

### 5.2 What the test does and where stock ionic + emulator fail

`ncdevmem -l -s <ip> -p <port> -f IF -v 7` (server), stock driver and bd902345 emulator:

| step | source | result today |
| --- | --- | --- |
| count RX queues, need >= 2 | NC:1518-1524 | ok (emulator offers 4) |
| read ring config | NC:873 | ok |
| `tcp-data-split on` | NC:879, rings.c:165 | **fails** `-EOPNOTSUPP` |
| `ethtool -X IF equal N` | NC:885 | "succeeds"; emulator ignores it |
| `ethtool -N ... queue N` | NC:891 | **fails**, no `set_rxnfc` |
| `bind-rx` | NC:896 | **fails** `-EOPNOTSUPP` at netdev-genl.c:1088; behind it queue ops, HDS config, `-ENODATA` from the pool check |
| receive and validate | NC:955-1045 | would fail: `CHECKSUM_NONE` on unreadable payload |

`devmem.py` additionally probes with `run_devmem_tests()` first (NC:1103-1170): `ethtool -X`,
HDS on/off/on with binds that must fail and succeed, and a channel shrink that must be
refused.

### 5.3 What must be true for the smallest test

- Receiver guest kernel: `CONFIG_NET_DEVMEM`, `CONFIG_UDMABUF`, lockdep recommended; ionic
  with items 1-7 (item 6 as the stub).
- Emulator: RX checksum and Option A HDS.
- Receiver: `ethtool -L IF combined 1`; `ethtool -G IF tcp-data-split on`;
  `ncdevmem -n -l -s <ip> -p 5201 -f IF -t 0 -q 1 -v 7 -L`.
- Sender: plain TCP is enough, e.g. the selftest's pattern generator piped into `nc`
  (`selftest-ncdevmem.md` section 3). devmem TX on the sender is orthogonal and can replace it
  once the TX PoC runs.
- The ionic netdev must carry only the test flow, ARP, neighbour discovery and small control packets; SSH must
  be on another NIC (INFERRED from 1.5; the VM stack's management path was not re-checked
  here).

## 6. Recommendation

**Go, as an emulator-backed PoC, with the scope stated plainly: it demonstrates the Linux
side and the shape of a device interface; it does not show that any ionic hardware can do
devmem RX.** Do not pursue Option B as a deliverable.

No-go conditions that would change this: if ops-locking ionic turns out to need the RDMA
auxiliary driver or FW-recovery paths reworked beyond the estimate, RX should be parked and
the TX series sent alone.

### 6.1 Ordered plan

1. Emulator: RX checksum (`CSUM_F_CALC`), grant `RX_CSUM`. Check plain TCP in a guest still
   works and no "hw csum failure" is logged. Valuable on its own.
2. Emulator: Option A HDS behind an experimental profile key (2X completions, parser, split
   placement), unit-tested against golden frames: TCP v4/v6 with and without options, pure
   ACK, VLAN, fragment, UDP, runt, header longer than buffer 0.
3. Driver: interface definitions, HDS datapath and ethtool knob, tested with ordinary pages:
   `tcp-data-split on`, iperf/nc correctness, confirm `skb_headlen == hds_hdr_len`.
4. Driver: netmem conversion (no behaviour change; rerun 3).
5. Driver: ops locking, under lockdep, cycling ifup/ifdown, ring resize, channel changes,
   link flap.
6. Driver: queue ops (stub) and pool flags.
7. First light: section 5.3. Evidence: ncdevmem validation passes with `-L`,
   `page-pool-get` shows the dmabuf id on the queue, emulator split counters match.
8. Emulator RSS indirection, then the flow filter; driver `rxnfc`; run stock
   `ncdevmem`/`devmem.py check_rx` with two or more queues.
9. Replace the queue-op stub with a true single-queue restart; guest A devmem TX -> guest B
   devmem RX.

### 6.2 Questions only real firmware can answer

1. Whether any firmware has or plans RX header split, how it is negotiated, and where the
   header length is reported.
2. Whether a small buffer 0 is accepted with SG, and the guaranteed fill order across
   buffer 0 and SG elements.
3. Whether `Q_CONTROL(DISABLE)` is synchronous (no DMA into old buffers after it completes),
   and whether one RX queue may be re-`Q_INIT`ed while the LIF is up and not quiesced. For
   devmem a late write lands in memory already handed back to userspace.
4. Whether a flow-steering filter exists or can exist behind `RX_FILTER_ADD`, its capacity,
   and its priority against RSS.
5. `comp.csum` semantics with split placement, VLAN stripping and odd lengths, and whether
   `CSUM_F_CALC` is set for every TCP segment.
6. Whether per-queue feature sets may differ within a LIF (HDS on one queue only), and the
   cost of 2X completions.
7. Whether the device can DMA to the addresses a GPU dma-buf exports (P2P, IOMMU).
8. Split behaviour for encapsulated traffic (inner or outer headers) and for CMB RX rings.
9. Limits: 8 SG elements of at most 64K each; whether larger payload buffers
   (`QCFG_RX_PAGE_SIZE`) are possible.

## 7. Not done / not verified

- Nothing compiled, booted or run. The build containers `devmem-kbuild-7-2-15` and
  `devmem-kbuild-7-4-rc3` seen running belong to other steps and were not touched.
- Emulator line numbers are for commit bd902345; the working tree differs (see the prefix
  list).
- GRO handling of unreadable skbs and the page_pool frag allocator's behaviour with net_iovs
  were not traced in detail.
- The lock-order claim in 2.6 is from reading, not from lockdep.
- LOC and risk figures are estimates.
- The earlier notes (`ernic-vm-stack.md`, `selftest-ncdevmem.md`, `ernic-emu-datapath.md`)
  were used for VM topology and ncdevmem facts; their kernel-config and host-readiness claims
  were not re-verified here.

## Verification pass

Independent re-read, 2026-11-18, by a second reviewer trying to refute the study. Read-only:
nothing built, booted or run; only this section was added. Kernel core read from
`/var/tmp/ionic-devmem/linux` (v7.4-rc3; core files unmodified in the working tree), ionic
from `git show HEAD:` (pristine v7.4-rc3), emulator from `git show HEAD:` (bd902345) and
spot-checked against the working tree. Where this section conflicts with the text above, this
section wins.

### Core requirements for bind-rx

- CONFIRMED. Ops-lock gate: `netdev-genl.c:1088-1089` and `netdev_rx_queue.c:251-252`;
  `netdev_need_ops_lock()` is `request_ops_lock || queue_mgmt_ops` (+ net_shaper)
  (`include/net/netdev_lock.h:28-37`). No `netdev_lock|ops_lock|queue_mgmt` in
  `drivers/net/ethernet/pensando/`.
- CONFIRMED. `queue_mgmt_ops` (`netdev_rx_queue.c:196`) and all four callbacks (`:108-110`).
- CONFIRMED. `hds_config == ENABLED` (`:199`), `hds_thresh == 0` (`:203`), no XDP (`:207`);
  setting the ring attribute needs `ETHTOOL_RING_USE_TCP_DATA_SPLIT`
  (`net/ethtool/rings.c:165-170`), which ionic lacks (`ionic_ethtool.c:1150-1151`).
- CONFIRMED. Pool gate: `page_pool_check_memory_provider()` returns `-ENODATA` unless a pool
  with `mp_priv == binding` and `slow.queue_idx == idx` exists (`page_pool_user.c:401-423`),
  run after `ndo_queue_mem_alloc` and before stop/start (`netdev_rx_queue.c:124-137`).
  `page_pool_init()` copies the provider only with `PP_FLAG_ALLOW_UNREADABLE_NETMEM`, under
  `netdev_assert_locked()` (`page_pool.c:275-281`), and needs `dma_map` and `dma_sync`
  (`:283-287`). ionic's pool flags as stated (`ionic_lif.c:591-601`).
- CONFIRMED. The core checks configuration only; nothing verifies a real split. No extra
  check hides in `net_devmem_bind_dmabuf_to_queue()` (`devmem.c:152-182`).
- CONFIRMED. `netdev_rx_queue.c` and `page_pool.c` are byte-identical at v7.2.15 (`cmp`);
  `netdev_rx_queue.c` also at v7.2.6. ionic `ionic_txrx.c` identical v7.4-rc3 vs v7.2.15.
- ADDED (not in the study). The queue callbacks run under the netdev instance lock **without
  RTNL** (`netdev_nl_bind_rx_doit()` takes only `netdev_get_by_index_lock()`,
  `netdev-genl.c:1081`; 1d22d3060b9b "net: drop rtnl_lock for queue_mgmt operations"). A stub
  that reuses `ionic_reconfigure_queues()` runs code that has only ever run under RTNL.
  Whether anything in it depends on RTNL: UNVERIFIABLE by reading alone; needs lockdep.
- ADDED. When the netdev is down the core frees the *new* memory after the pool check
  (`netdev_rx_queue.c:140-144`), so `mem_alloc` must still create a provider pool while down,
  or fail the bind as bnxt does. ionic frees its qcqs in `ionic_stop()`
  (`ionic_lif.c:2354-2356`), so the PoC must bind with the interface up.

### RX checksum prerequisite

- CONFIRMED. `skb_checksum()` warns and returns 0 on unreadable frags (`skbuff.c:3570-3571`);
  ionic sets only `CHECKSUM_COMPLETE` from `CSUM_F_CALC` (`ionic_txrx.c:684-691`); the
  emulator writes `csum_flags = 0` at bd902345 (`ionic_eth_emu.c:2224-2233`) and still does in
  the working tree (`:2564`); the working tree's feature mask has no RX bit
  (`ionic_eth_emu_priv.h:453-455`).
- WRONG by omission. `CHECKSUM_COMPLETE` is weaker than what the reference drivers use: bnxt,
  gve and fbnic report `CHECKSUM_UNNECESSARY` (`bnxt.c:2064`, `gve_rx_dqo.c:472`,
  `fbnic_txrx.c:306`). With `CHECKSUM_COMPLETE`, any trim of trailing bytes
  (`pskb_trim_rcsum_slow()` -> `pskb_trim_rcsum_complete()`, `skbuff.c:2854-2860`) calls
  `skb_checksum()` over the tail. If the device split a padded runt (TCP payload of 1-5 bytes
  without timestamps, frame padded to 60), the pad sits in an unreadable frag and the
  `WARN_ON_ONCE` fires. Correction to Option A rule 2: do not split a frame longer than
  L2 header + IP total length (it fits buffer 0 anyway). INFERRED, not run.
- UNVERIFIABLE. The byte-order argument in 4.3 (store the big-endian-word sum high byte
  first). I re-derived it and agree, but it must be tested.

### Option B (small buffer 0, no device change)

- CONFIRMED that it passes the core and works on the unmodified emulator: buffer 0 is filled
  to its posted length, then SG elements in order, no minimum (`ionic_eth_emu.c:2153-2190,
  2208-2218`).
- CONFIRMED that it cannot give devmem semantics. I looked for a rescue and found none: the
  payload prefix in buffer 0 cannot be attached as a page frag next to net_iov frags
  (`netmem.rst:68-73`; `tcp_recvmsg_dmabuf()` returns `-ENODEV` for a non-net_iov frag,
  `tcp.c:2581-2584`), so it must go to the linear area and comes out as `SCM_DEVMEM_LINEAR`
  (`tcp.c:2531-2558`). ncdevmem skips linear bytes and fails with `-L` (NC:991-1004), and
  `validate_buffer()` keeps a running seed (NC:278-301), so `-v` desynchronises.
- CONFIRMED (by reasoning) that sizing buffer 0 to the data-segment header breaks the SYN.

### Single RX queue restart in ionic

- CONFIRMED. Per-queue `Q_CONTROL` and `Q_INIT` exist (`ionic_lif.c:292-383, 898-985`), no
  per-queue deinit command (`:385-403`), no existing path restarts one data queue; the
  hwstamp queue is the only queue brought up while others run (`:1087-1098`).
- CONFIRMED. RX i and TX i share `ionic_txrx_napi` unless split-interrupt (`:972-975`), so a
  real per-queue restart must quiesce TX i.
- CONFIRMED. `ionic_reconfigure_queues()` allocates replacement qcqs with their own
  page_pools before stopping (`:3018-3050`), without interrupts (`flags & ~IONIC_QCQ_F_INTR`,
  `:3039`), then swaps (`:2902-2944`). That maps onto mem_alloc/stop/start/mem_free.
- CONFIRMED for the emulator: `Q_INIT` replaces one queue's state and `Q_CONTROL` toggles one
  queue (`ionic_eth_emu.c:1742-1752, 1803-1809`); the emulator is single-threaded on this
  path, so disable is synchronous there. Real firmware: UNVERIFIABLE.
- ADDED. `ionic_start_queues_reconfig()` restarts through the deferred link-check work
  (`CAN_NOT_SLEEP`, `ionic_lif.c:1790`). In the stub, `ndo_queue_start` therefore returns
  before any queue runs, and the work then needs the instance lock the bind still holds.
  Workable, but the stub should start queues synchronously instead.

### Emulator steering

- CONFIRMED. 4 queues by default (`ionic_profile.c`, `p->queues.eth = 4`); steering is
  `active[hash % n]` over enabled queues (`ionic_eth_emu.c:2291-2311`); `LIF_SETATTR` handles
  only MAC and FEATURES, so the RSS table is ignored; `RX_FILTER_ADD` has no state
  (`:1867-1870`). ionic has no rxnfc ops (`ionic_ethtool.c:1146-1182`).
- WRONG in part: "not controllable" (4.4). The guest cannot control it, but the **sender**
  can: the hash is unkeyed FNV-1a over addresses, protocol and ports
  (`ionic_eth_steer.c:48-78`), so a chosen source port puts the test flow on a chosen queue
  with no emulator change. It does not keep other flows off that queue, so it is a test
  convenience for a two-queue run, not a substitute for the filter.
- ADDED. The flow-filter emulator work has no estimate in the study. It needs unique filter
  ids and `RX_FILTER_DEL` handling: today every `RX_FILTER_ADD` completes with a zeroed
  completion, i.e. `filter_id` 0, and the driver keys its filter list on that id
  (`ionic_rx_filter.c:165-178, 505`).

### Ops locking

- CONFIRMED. `napi_enable()`/`napi_disable()` take `netdev_lock()` (`dev.c:7908-7948`),
  `netif_napi_add()` and `__netif_napi_del()` likewise (`netdevice.h:2932-2944, 3007-3012`);
  ionic calls them from paths the core would hold the lock across (`ionic_lif.c:325, 367,
  399, 973-978`). The deadlock is INFERRED, not run.
- WRONG by omission: the affected ndo list is longer than "open/stop/ethtool". Also
  `ndo_change_mtu` (`:1811`, via stop/start reconfig), `ndo_bpf` (`:2764-2815`), and
  `ndo_hwtstamp_set` (`ionic_phc.c:133-142` -> `ionic_lif_create_hwstamp_{tx,rx}q()`, which
  call `netif_napi_add()` and `napi_enable()`). The same helpers also run unlocked from work
  items (link check `:165-175`, FW up `:3459`), and `ionic_restart_lif()` re-creates the
  adminq NAPI (`:3610-3612`) inside the region that would need the lock.
- UNVERIFIABLE. The 100-200 line figure. As context, bnxt and gve each needed several
  follow-up deadlock fixes after converting (e4546c6498c6, db807e5ef8ee, 325eb217e41f,
  dd07df9ff3d1, 9e3267cf02c2). Treat the estimate as a floor and the risk as correctly "high".
- UNVERIFIABLE. Interaction with the RDMA auxiliary driver. `drivers/infiniband/hw/ionic`
  does not touch NAPI or the instance lock itself, but it is loaded on this same netdev in
  the test stack, so the PoC exercises it whether or not anyone looks.

### Option A interface

- CONFIRMED. `BIT_ULL(17)` is unused in the queue feature enums (`ionic_if.h:409-441`);
  `0x11` is unused in `ionic_rx_filter_match_type` (`:2059-2062`); LIF `BIT(21)` is taken at
  v7.4 (`:1213`) and absent at v7.2.15 (last is BIT(20), `:1183`); `ionic_rxq_comp` is
  16 bytes with no spare; the 2X completion puts the standard half last
  (`ionic_txrx.c:733-738`); `Q_INIT.features` is at offset 56 (`ionic_if.h:767-790`).
- UNVERIFIABLE. That none of these values collide with out-of-tree or firmware definitions.
  The study already says not to upstream them.

### Reference drivers, test flow

- CONFIRMED. All 25 cited hashes exist with the stated subjects and shortstats
  (`git show --shortstat`).
- CONFIRMED. ncdevmem `-n` skips header split, RSS and flow steering (NC:872-895, 1463-1465);
  the two-queue minimum applies only without `-t`/`-q` (NC:1518-1534).
- Not re-checked: netdevsim details, the mlx5/fbnic code citations in section 3, v7.2.6
  `ionic_txrx.c` differences, sections 2.4 and 2.7 beyond what is cited here.

### Effort estimates

- WRONG (arithmetic). The headline "about 700-1000 lines" does not match the table. Items
  1-7 with the stub sum to 610-800; with docs 660-850; with real queue ops and n-tuple
  1140-1330. Use: **first light about 650-850 driver lines; stock-selftest capable about
  1150-1350.**
- UNVERIFIABLE. Emulator sizes. Under rocm-ernic's rules (tests for every error path,
  coverage floor, golden frames, changelog, docs) expect the tests to exceed the 300-450
  lines of C, and add the unestimated RSS-table and flow-filter work.

### Go/no-go

- CONFIRMED: GO as an emulator-backed PoC, Option A, Option B not a deliverable.
- CHANGED: the plan's order. Ops locking is the largest risk, needs no device interface, is
  independently upstreamable, and decides the no-go condition. It was step 5; it should be
  first, run under lockdep against today's emulator (with the RDMA driver loaded) before any
  emulator HDS work is spent.
- CHANGED: the driver should not rely on `CHECKSUM_COMPLETE` alone on HDS queues (above);
  either the device rule excludes padded frames from splitting, or the driver gains
  `CHECKSUM_UNNECESSARY` from `CSUM_F_TCP_OK`.
- CHANGED: an intermediate two-queue run is possible before the flow filter exists, by
  choosing the sender's source port.
