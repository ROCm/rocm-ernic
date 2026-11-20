# ionic Ethernet TX path audit for devmem TX (netmem_tx)

Read-only audit, 2026-11-18. Trees:

- `74:`   = `/var/tmp/ionic-devmem/linux` at tag **v7.4-rc3** (mainline target)
- `726:`  = `/var/tmp/ionic-devmem/linux-7.2.6` at tag **v7.2.6** (ionic source pin for the DKMS module)
- `7215:` = `/var/tmp/ionic-devmem/linux-7.2.15` at tag **v7.2.15** (kernel the DKMS module builds against)

Driver paths are relative to `drivers/net/ethernet/pensando/ionic/`. Core paths are relative to the
tree root. Every claim is tagged **VERIFIED** (read in source at the cited tag) or **INFERRED**.

## 0. Decisive facts

1. VERIFIED: `skb_frag_dma_map()` already handles net_iov frags in both 7.2.15 and 7.4-rc3, so
   `ionic_tx_map_frag()` maps a devmem frag correctly today. **Only the unmap side is wrong**:
   four `dma_unmap_page()`/`dma_unmap_single()` sites, two functions (section 3.3).
2. VERIFIED: ionic has **no `ndo_fix_features` and no `ndo_features_check`**, never calls
   `netif_set_tso_max_*`, and never pads or touches payload on TX. The only payload-touching call
   in the whole TX path is the `skb_linearize()` fallback in `ionic_tx_descs_needed()`.
3. VERIFIED: that `skb_linearize()` returns `-ENOMEM` for an unreadable skb (`__pskb_pull_tail()`
   returns NULL), which `ionic_start_xmit()` already turns into `q->drop++; dev_kfree_skb()`.
   So the frag-limit overflow already "fails cleanly" in the driver without a code change; it is a
   silent, persistent drop (TCP retransmits the same layout), visible only in debugfs.
4. VERIFIED: the driver **ignores the device-reported `max_sg_elems`** and forces 15
   (`min(IONIC_TX_MAX_FRAGS - 1, MAX_SKB_FRAGS)`), for v0 (8-element, 128-byte SG) queues too.
   The 4-bit NSGE field in `cmd` caps at 15. An skb has up to `MAX_SKB_FRAGS` = 17 frags.
5. VERIFIED: with SG **and** HW_CSUM both off (device grants neither, or user turns both off), a
   TCP GSO devmem skb is software-segmented by the core with **zero-filled payload** and sent
   (`skb_copy_and_csum_bits()`), which is not a clean failure. Every other feature state drops
   cleanly in `validate_xmit_skb()` before the driver. This is the argument for gating
   `netmem_tx` on SG+CSUM being granted.
6. VERIFIED: `ionic_txrx.c`, `ionic_dev.h`, `ionic_stats.c`, `ionic_dev.c` differ between v7.2.6 and
   v7.4-rc3 only in the tail of `ionic_tx()` (+7 lines). A devmem patch that stays out of
   `ionic_tx()` lines 1672-1692 applies to v7.2.6 with line offsets only. No rocm-ernic patch
   touches `ionic_txrx.c`, `ionic_lif.c`, or `ionic_dev.h`.

---

## 1. Checksum and SG offload

### 1.1 Negotiation (VERIFIED)

`ionic_init_nic_features()` asks for everything, `74:ionic_lif.c:1574-1602` (`726:1571-1599`):

```c
	features = NETIF_F_HW_VLAN_CTAG_TX | NETIF_F_HW_VLAN_CTAG_RX | NETIF_F_HW_VLAN_CTAG_FILTER |
		   NETIF_F_SG | NETIF_F_HW_CSUM | NETIF_F_RXCSUM |
		   NETIF_F_TSO | NETIF_F_TSO6 | NETIF_F_TSO_ECN |
		   NETIF_F_GSO_GRE | ... | NETIF_F_GSO_UDP_TUNNEL_CSUM;
	...
	err = ionic_set_nic_features(lif, features);
```

`ionic_set_nic_features()` translates with `ionic_netdev_features_to_nic()`
(`74:ionic_lif.c:1455-1493`: `NETIF_F_SG -> IONIC_ETH_HW_TX_SG`, `NETIF_F_HW_CSUM ->
IONIC_ETH_HW_TX_CSUM`, `NETIF_F_TSO -> IONIC_ETH_HW_TSO`, `NETIF_F_TSO6 -> IONIC_ETH_HW_TSO_IPV6`,
...), posts `IONIC_CMD_LIF_SETATTR / IONIC_LIF_ATTR_FEATURES`, and keeps the intersection,
`74:ionic_lif.c:1523-1525` (`726:1522-1524`):

```c
	old_hw_features = lif->hw_features;
	lif->hw_features = le64_to_cpu(ctx.cmd.lif_setattr.features &
				       ctx.comp.lif_setattr.features);
```

Feature bits: `74:ionic_if.h:1191-1214` (`IONIC_ETH_HW_TX_SG = BIT(5)`, `IONIC_ETH_HW_TX_CSUM =
BIT(7)`, `IONIC_ETH_HW_TSO = BIT(8)`, `IONIC_ETH_HW_TSO_IPV6 = BIT(9)`, ...).

Netdev features are then built only from granted bits, `74:ionic_lif.c:1604-1643`
(`726:1601-1640`):

```c
	netdev->features |= NETIF_F_HIGHDMA;
	...
	if (lif->hw_features & IONIC_ETH_HW_TX_SG)
		netdev->hw_features |= NETIF_F_SG;

	if (lif->hw_features & IONIC_ETH_HW_TX_CSUM)
		netdev->hw_enc_features |= NETIF_F_HW_CSUM;
	...
	if (lif->hw_features & IONIC_ETH_HW_TSO)
		netdev->hw_enc_features |= NETIF_F_TSO;
	if (lif->hw_features & IONIC_ETH_HW_TSO_IPV6)
		netdev->hw_enc_features |= NETIF_F_TSO6;
	...
	netdev->hw_features |= netdev->hw_enc_features;
	netdev->features |= netdev->hw_features;
	netdev->vlan_features |= netdev->features & ~NETIF_F_VLAN_FEATURES;
```

Consequences (VERIFIED):

- A feature the device does not grant is in neither `hw_features` nor `features`: off and **fixed**
  (not user-toggleable).
- A granted feature is in `hw_features`, so the user can toggle it with `ethtool -K`.
- `netdev_ops` has `.ndo_set_features = ionic_set_features` and **no `.ndo_fix_features`, no
  `.ndo_features_check`** (`74:ionic_lif.c:2821-2846`, `726:2818-`).
- `ionic_set_features()` (`74:ionic_lif.c:1657-1669`, `726:1654-`) just re-runs
  `ionic_set_nic_features()` and returns its error. It does not compare the grant with the
  request, so after a toggle `netdev->features` can say "on" for a bit the device refused; the
  mismatch is only in `lif->hw_features`.
- The TX path never consults `lif->hw_features` or `netdev->features`: it acts on the skb alone
  (`skb->ip_summed == CHECKSUM_PARTIAL` at `74:ionic_txrx.c:1663`, `skb_is_gso()` at `:1850`).

Diff v7.2.6 -> v7.4-rc3 in this area (VERIFIED, `git diff v7.2.6 v7.4-rc3 -- ionic_lif.c`): only
the PHC timestamp bits in `ionic_set_nic_features()` (`lif->phc` now ORs in identity-masked
`IONIC_ETH_HW_TIMESTAMP | IONIC_ETH_HW_RDMA_TIMESTAMP`, +1 line at 1513-1517) and one more
`dev_dbg` (+2 lines at 1568-1569). `ionic_init_nic_features()` and `ionic_set_features()` bodies
are byte-identical, shifted by +3 lines.

### 1.2 What a devmem skb experiences in each state

Core facts used below (VERIFIED at both `74:` and `7215:` unless noted):

- TCP always gives itself SG|HW_CSUM and software GSO regardless of the device:
  `sk->sk_route_caps |= NETIF_F_SG | NETIF_F_HW_CSUM;` (`74:net/core/sock.c:2691`,
  `7215:net/core/sock.c:2662`). So `sendmsg(MSG_ZEROCOPY + SCM_DEVMEM_DMABUF)` succeeds and builds
  (possibly GSO) unreadable CHECKSUM_PARTIAL skbs whatever `dev->features` says; the `-EINVAL` at
  `74:net/ipv4/tcp.c:1178` (`7215:net/ipv4/tcp.c:1172`) is not reached for TCP.
- `validate_xmit_skb()` (`74:net/core/dev.c:4202-4270`, `7215:net/core/dev.c:4081-`):
  `validate_xmit_unreadable_skb()` first (drops if `dev->netmem_tx == NETMEM_TX_NONE` or the
  binding's dev differs, `74:net/core/dev.c:4169-4196`, identical in 7215 at `:4048-`), then
  `netif_needs_gso()` -> `__skb_gso_segment()` (`74::4238`), else `skb_needs_linearize() &&
  __skb_linearize()` (`74::4246-4247`, `7215::4109-4110`) and `skb_csum_hwoffload_help()`
  (`74::4261`, `7215::4124`).
- `__skb_linearize()` is `__pskb_pull_tail(skb, skb->data_len) ? 0 : -ENOMEM`
  (`74:include/linux/skbuff.h:4023-4026`) and `__pskb_pull_tail()` starts with
  `if (!skb_frags_readable(skb)) return NULL;` (`74:net/core/skbuff.c:2898-2907`,
  `7215:net/core/skbuff.c:2878-2887`).
- `skb_checksum_help()`: `if (!skb_frags_readable(skb)) { return -EFAULT; }`
  (`74:net/core/dev.c:3679-3681`, `7215:net/core/dev.c:3624-3626`).
- `skb_segment()` (`74:net/core/skbuff.c:4857`): `sg = !!(features & NETIF_F_SG); csum =
  !!can_checksum_protocol(features, proto);` (`:4906-4907`). With `!sg`:

  ```c
		if (!sg) {
			if (!csum) {
				...
				SKB_GSO_CB(nskb)->csum =
					skb_copy_and_csum_bits(head_skb, offset,
							       skb_put(nskb, len), len);
			} else {
				if (skb_copy_bits(head_skb, offset, skb_put(nskb, len), len))
					goto err;
			}
			continue;
		}
  ```
  (`74:net/core/skbuff.c:5068-5083`). `skb_copy_bits()` faults on unreadable (`:3062`), but
  `skb_copy_and_csum_bits()` does not fail:

  ```c
	if (!skb_frags_readable(skb)) {
		/* Don't hand the caller a buffer with stale bytes in it. */
		if (len > 0)
			memset(to, 0, len);
		return 0;
	}
  ```
  (`74:net/core/skbuff.c:3659-3664`; same text in `7215:net/core/skbuff.c:3636-`; from commit
  ab9414ed70bd, whose log says "The checksum usually won't match the data any more, so the
  receiver will usually drop the packet").
  With `sg && !csum`, `perform_csum_check` does `if (skb_has_shared_frag(nskb) &&
  __skb_linearize(nskb)) goto err;` (`74:net/core/skbuff.c:5182-5186`); msg_zerocopy skbs carry
  `SKBFL_SHARED_FRAG` (`SKBFL_ZEROCOPY_FRAG`, `74:net/core/skbuff.c:1750`), so this errors out.
- `netdev_fix_features()` drops TSO when SG is off and TSO/TSO6 when no csum
  (`74:net/core/dev.c:11192-11208`). It has **no** "SG needs checksum" rule, so
  `ethtool -K tx off` leaves SG on.

| State | non-GSO devmem skb | GSO devmem skb | Clean? |
| --- | --- | --- | --- |
| SG + HW_CSUM + TSO on (all granted) | reaches `ionic_tx()`: `CSUM_PARTIAL` opcode with `csum_start`/`csum_offset` (`74:ionic_txrx.c:1575-1587`), frags as SG elems | reaches `ionic_tx_tso()` | works |
| SG + HW_CSUM on, TSO off (not granted, or `ethtool -K tso off`) | as above | core `skb_segment()` with sg && csum: segments share frags by reference, stay CHECKSUM_PARTIAL, `unreadable` copied with the header group (`74:include/linux/skbuff.h:976-1093`, `74:net/core/skbuff.c:1570`); each reaches `ionic_tx()` | works (INFERRED for `unreadable` propagation: bit is inside `struct_group(headers)`, not tested) |
| SG on, HW_CSUM off (`ethtool -K tx off`; TSO auto-dropped) | `skb_csum_hwoffload_help()` -> `skb_checksum_help()` -> `-EFAULT` -> `kfree_skb`, `tx_dropped++` | `skb_segment()` sg && !csum -> `__skb_linearize(nskb)` fails -> `goto err` -> drop | clean drop, TCP stalls |
| SG off, HW_CSUM on (`ethtool -K sg off`; TSO auto-dropped) | `__skb_linearize()` fails -> drop | `skb_segment()` !sg && csum -> `skb_copy_bits()` `-EFAULT` -> drop | clean drop, TCP stalls |
| SG off, HW_CSUM off (device grants nothing = rocm-ernic today; or user turns both off) | `__skb_linearize()` fails -> drop | `skb_segment()` !sg && !csum -> **segments emitted with zero-filled payload** and a checksum computed from csum 0 | **NOT clean** (INFERRED on-wire result: bad L4 checksum, receiver drops; data is zeros, not leaked) |

In every non-working state `sendmsg()` still returns success; the symptom is a stalled connection
plus `tx_dropped`, never an errno (matches W1 in `ernic-emu-datapath.md`).

The driver itself is never reached with an unreadable skb in the bad states, so nothing in
`ionic_txrx.c` can make these cleaner; the lever is whether `netmem_tx` is set.

### 1.3 Proposal: fail cleanly

1. **Set `netdev->netmem_tx = NETMEM_TX_DMA` only when the device granted SG and HW_CSUM**, in
   `ionic_init_nic_features()` after `netdev->features |= netdev->hw_features`
   (`74:ionic_lif.c:1642`, `726:1639`):

   ```c
	if ((lif->hw_features & (IONIC_ETH_HW_TX_SG | IONIC_ETH_HW_TX_CSUM)) ==
	    (IONIC_ETH_HW_TX_SG | IONIC_ETH_HW_TX_CSUM))
		netdev->netmem_tx = NETMEM_TX_DMA;
   ```
   Without it `NETDEV_CMD_BIND_TX` fails with `-EOPNOTSUPP` "Driver does not support netmem TX"
   (`74:net/core/netdev-genl.c:1241-1246`), which is the clean, user-visible failure. This also
   removes the zero-fill row above for the "device grants nothing" case. `netmem_tx` is a 2-bit
   enum field in both kernels (`74:include/linux/netdevice.h:1822-1826,2180`,
   `7215:include/linux/netdevice.h:1811-,2166`); no compat shim needed.
   `ionic_init_nic_features()` runs before `register_netdev` (INFERRED from call order via
   `ionic_lif_init()`; not re-read), same place mlx5/bnxt/fbnic set it.
2. **Runtime `ethtool -K` of sg/tx**: other netmem_tx drivers (mlx5 `en_main.c:5972`, bnxt
   `bnxt.c:17474`, gve `gve_main.c:3080`, fbnic `fbnic_netdev.c:762`, all `74:`) set `netmem_tx`
   unconditionally and do nothing at toggle time; they rely on the core backstops in the table.
   Options for ionic, in order of preference:
   - (a) Do nothing more (upstream precedent). Rows 3 and 4 already drop cleanly. Only "both off
     by the user" hits the zero-fill path, and that is a core property shared by every netmem_tx
     driver, better fixed in `skb_segment()` (bail for an unreadable `head_skb` when `!sg`) than
     in ionic.
   - (b) In `ionic_set_features()`, recompute `netdev->netmem_tx` from the requested `features`
     (`NETIF_F_SG && NETIF_F_HW_CSUM`). Then `validate_xmit_unreadable_skb()` frees in-flight
     unreadable skbs (`74:net/core/dev.c:4179-4180,4193-4195`) and new binds fail. Costs: a
     non-atomic RMW of a bitfield word that the xmit path reads locklessly, and no precedent;
     reviewers will likely push back. Reasonable for the PoC only if the "both off" case must be
     closed in-driver.
   - (c) `ndo_fix_features` cannot help: it cannot know whether a TX binding exists, and refusing
     to clear SG/CSUM for everyone is a behaviour change.
3. Do not force features the device did not grant. With rocm-ernic that means the emulator must
   grant `IONIC_ETH_HW_TX_SG | IONIC_ETH_HW_TX_CSUM` in the LIF_SETATTR(FEATURES) completion and
   implement CSUM_PARTIAL (see `ernic-emu-datapath.md` F1/F2); TSO can stay ungranted (row 2).

---

## 2. Frag limit vs TSO

### 2.1 Where `max_sg_elems` comes from (VERIFIED)

- Wire limits, `74:ionic_if.h:1016-1026` (`726:ionic_if.h:987-997`):

  ```c
  struct ionic_txq_sg_desc {
  #define IONIC_TX_MAX_SG_ELEMS		8
  #define IONIC_TX_SG_DESC_STRIDE		8
	struct ionic_txq_sg_elem elems[IONIC_TX_MAX_SG_ELEMS];
  };
  struct ionic_txq_sg_desc_v1 {
  #define IONIC_TX_MAX_SG_ELEMS_V1		15
  #define IONIC_TX_SG_DESC_STRIDE_V1		16
	struct ionic_txq_sg_elem elems[IONIC_TX_SG_DESC_STRIDE_V1];
  };
  ```
  `static_assert(sizeof(struct ionic_txq_sg_desc) == 128)`, `_v1 == 256`
  (`74:ionic_dev.h:130-131`).
- NSGE is 4 bits of `cmd`: `IONIC_TXQ_DESC_NSGE_MASK 0xf`, shift 8 (`74:ionic_if.h:944-945`);
  `encode_txq_desc_cmd()` masks silently: `cmd |= (nsge & IONIC_TXQ_DESC_NSGE_MASK) << ...`
  (`74:ionic_if.h:985`). Hard ceiling 15 SG elems + 1 head = 16 buffers per descriptor.
- `#define IONIC_TX_MAX_FRAGS (1 + IONIC_TX_MAX_SG_ELEMS_V1)` = 16 (`74:ionic_dev.h:213`).
- The driver requests TXQ version 3 (`74:ionic_lif.c:28-37`: "1 = ... with Tx SG version 1,
  3 = ... with CMB rings") via Q_IDENTIFY and reads `max_sg_elems`, `sg_desc_sz` from the reply
  (`74:ionic_lif.c:3938-3950`).
- **Driver override**: the reported value is then discarded, `74:ionic_lif.c:3982-3991`
  (`726:3979-3988`):

  ```c
		if (qtype == IONIC_QTYPE_TXQ)
			max_frags = IONIC_TX_MAX_FRAGS;
		else if (qtype == IONIC_QTYPE_RXQ)
			max_frags = IONIC_RX_MAX_FRAGS;
		else
			max_frags = 1;

		qti->max_sg_elems = min_t(u16, max_frags - 1, MAX_SKB_FRAGS);
  ```
  = `min(15, MAX_SKB_FRAGS)` = **15** with the default `MAX_SKB_FRAGS` 17
  (`74:include/linux/skbuff.h:350-354`). Introduced by 4dcd4575bfb1 ("ionic: use specialized desc
  info structs", 2024-03-06), which replaced a clamp of the reported value with this assignment.
  Copied to the queue at `74:ionic_lif.c:616` (`new->q.max_sg_elems =
  lif->qtype_info[type].max_sg_elems`).
- SG descriptor size is chosen separately and does honour the device,
  `74:ionic_lif.c:2091-2096` (`726:2088-2093`; same test at `:1004-1008` for the hwstamp txq and
  `:2989-2994` in reconfigure):

  ```c
	if (lif->qtype_info[IONIC_QTYPE_TXQ].version >= 1 &&
	    lif->qtype_info[IONIC_QTYPE_TXQ].sg_desc_sz ==
					  sizeof(struct ionic_txq_sg_desc_v1))
		sg_desc_sz = sizeof(struct ionic_txq_sg_desc_v1);
	else
		sg_desc_sz = sizeof(struct ionic_txq_sg_desc);
  ```
- So on a v0 TXQ (8 elems, 128 bytes) the driver still allows `nsge` 9..15 and
  `ionic_tx_skb_frags()`/`ionic_tx_tso()` write past `elems[8]` into the next descriptor's SG
  slot. Pre-existing bug, not devmem-specific, but **rocm-ernic identifies v0 today** and drops
  `nsge > 8` (`ernic-emu-datapath.md` lines 126-150). The PoC needs either the emulator to
  identify TXQ v1 / `sg_desc_sz` 256, or a driver fix (cap at `IONIC_TX_MAX_SG_ELEMS` when
  `sg_desc_sz` is the v0 size).
- Per-descriptor buffer bookkeeping holds `MAX_SKB_FRAGS + 1` entries:
  `struct ionic_buf_info bufs[MAX_SKB_FRAGS + 1];` (`74:ionic_dev.h:222`), so mapping never
  overflows `bufs[]` whatever the SG limit.
- No `netif_set_tso_max_segs/size`, no `gso_max_*` anywhere in the driver (grep, `74:`); the
  netdev keeps the defaults (`TSO_MAX_SEGS`, 64K legacy sizes).

### 2.2 `ionic_tx_descs_needed()` (VERIFIED)

`74:ionic_txrx.c:1695-1781` (`726:1688-1774`, identical body).

Non-GSO, `74::1715-1726`:

```c
	} else {
		ndescs = 1;
		if (!nr_frags)
			return ndescs;

		if (unlikely(nr_frags > q->max_sg_elems)) {
			too_many_frags = true;
			goto linearize;
		}

		return ndescs;
	}
```
Triggers exactly when `nr_frags > 15`, i.e. **16 or 17 frags** with default `MAX_SKB_FRAGS`.

GSO: `ndescs = skb_shinfo(skb)->gso_segs` (`74::1712`), then a dry run of the TSO walk,
`74::1733-1770`:

```c
	seg_rem = min_t(int, tso_rem, hdrlen + skb_shinfo(skb)->gso_size);
	frag_rem = hdrlen;

	while (tso_rem > 0) {
		desc_bufs = 0;
		while (seg_rem > 0) {
			desc_bufs++;
			...
			if (desc_bufs > q->max_sg_elems + 1) {
				too_many_frags = true;
				goto linearize;
			}

			if (frag_rem == 0) {
				frag_rem = skb_frag_size(frag);
				frag++;
			}
			chunk_len = min(frag_rem, seg_rem);
			...
		}
		seg_rem = min_t(int, tso_rem, skb_shinfo(skb)->gso_size);
	}
```
Triggers when any one segment needs more than 16 buffers: the first segment counts the header
buffer plus every frag chunk it touches; later segments count frag chunks only.
The dry run assumes the linear area is exactly `hdrlen` (`frag_rem = hdrlen`); true for TCP
zerocopy/devmem skbs, whose head holds only headers (INFERRED from the zerocopy fill path putting
all payload in frags; if the head held payload this loop would miscount, a pre-existing issue).

Fallback, `74::1772-1778`:

```c
linearize:
	if (too_many_frags) {
		err = skb_linearize(skb);
		if (unlikely(err))
			return err;
		q_to_tx_stats(q)->linearize++;
	}
```
For an unreadable skb `skb_linearize()` returns `-ENOMEM` (section 1.2), the function returns a
negative `ndescs`, and both callers drop: `74:ionic_txrx.c:1841-1843,1860-1863` and
`:1796-1798,1814-1817`:

```c
	ndescs = ionic_tx_descs_needed(q, skb);
	if (ndescs < 0)
		goto err_out_drop;
	...
err_out_drop:
	q->drop++;
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
```
`q->drop` is exposed only in debugfs (`74:ionic_debugfs.c:145`), not in ethtool stats or
`ndo_get_stats64` (`74:ionic_lif.c:1314-1316` takes `tx_dropped` from firmware LIF stats).
The `linearize` ethtool counter (`74:ionic_stats.c:135`) is bumped only on success.

### 2.3 Trace of `ionic_tx_tso()` (VERIFIED)

`74:ionic_txrx.c:1435-1556` (identical at `726:`).

1. Pseudo-header checksum preload: `ionic_tx_tcp_inner_pseudo_csum()` / `ionic_tx_tcp_pseudo_csum()`
   (`:1470-1475`), headers only (section 3).
2. `ionic_tx_map_skb(q, skb, desc_info)` (`:1478`) maps the head and **all** frags once into
   `desc_info->bufs[]` of the first descriptor.
3. `tso_rem = len; seg_rem = min(tso_rem, hdrlen + mss);` (`:1496-1497`).
4. Outer loop, one descriptor per segment (`:1504-1548`). Inner loop (`:1511-1537`):

   ```c
			if (frag_rem == 0) {
				/* grab the next fragment */
				frag_addr = buf_info->dma_addr;
				frag_rem = buf_info->len;
				buf_info++;
			}
			chunk_len = min(frag_rem, seg_rem);
			if (!desc) {
				/* fill main descriptor */
				desc = &q->txq[q->head_idx];
				elem = ionic_tx_sg_elems(q);
				desc_addr = frag_addr;
				desc_len = chunk_len;
			} else {
				/* fill sg descriptor */
				elem->addr = cpu_to_le64(frag_addr);
				elem->len = cpu_to_le16(chunk_len);
				elem++;
				desc_nsge++;
			}
			frag_addr += chunk_len;
   ```
   The first chunk of each segment goes in the main descriptor (`cmd` address, 52-bit masked),
   each further chunk in an SG elem. A buffer larger than the remaining segment is split by
   address arithmetic (`frag_addr += chunk_len`), so one mapped buffer feeds many descriptors.
5. `ionic_tx_tso_post()` (`:1541-1543`, body `:1399-1433`) writes opcode TSO, SOT/EOT flags,
   `hdr_len`, `mss`; rings the doorbell only on the last descriptor.
6. After each post: `desc_info = &q->tx_info[q->head_idx]; desc_info->nbufs = 0;` (`:1546-1547`),
   so only the first descriptor of the TSO owns the buffers and the skb.

**The SG count is not bounded inside `ionic_tx_tso()`**: `desc_nsge` is a `u8` incremented
without a check, `elem++` walks past the array, and `encode_txq_desc_cmd()` would truncate nsge
to 4 bits. The only bound is the dry run in `ionic_tx_descs_needed()`, which mirrors this loop.
With the dry run in place, each descriptor has at most `max_sg_elems` (15) SG elems.
On a v0 queue the bound is wrong (15 instead of 8), see 2.1.

### 2.4 Worst cases

How devmem frags are built (VERIFIED, `74:net/core/datagram.c:707-744`, same in `7215:` apart from
the ref helper name): one frag per (iovec entry x dmabuf page), never coalesced, capped at
`MAX_SKB_FRAGS` per skb:

```c
	while (length && iov_iter_count(from)) {
		if (i == MAX_SKB_FRAGS)
			return -EMSGSIZE;

		virt_addr = (size_t)iter_iov_addr(from);
		niov = net_devmem_get_niov_at(binding, virt_addr, &off, &size);
		...
		size = min_t(size_t, size, length);
		size = min_t(size_t, size, iter_iov_len(from));
		...
		skb_add_rx_frag_netmem(skb, i, net_iov_to_netmem(niov), off, size, PAGE_SIZE);
```
with `*off = virt_addr % PAGE_SIZE; *size = PAGE_SIZE - *off;` (`74:net/core/devmem.c:460-461`).
`tcp_sendmsg_locked()` appends to the tail skb across calls when it is also unreadable
(`74:net/ipv4/tcp.c:1256-1257`), and `tcp_can_collapse()` refuses unreadable skbs
(`74:net/ipv4/tcp_output.c:3561`), so a retransmit keeps the same frag layout.

Bulk devmem send (the intended use), 4K pages, one large iovec at an arbitrary dmabuf offset:
frags are `[PAGE_SIZE - off, 4096, 4096, ..., tail]`, at most 17 per skb.

- MSS 1448: a segment touches at most 2 frags, 3 if a 1-byte edge frag sits inside it. Worst
  first segment = header + 3 chunks = 4 buffers. Limit is 16. **Never overflows.**
- MSS 8948 (MTU 9000): a segment spans at most 4 frags (e.g. 100 + 4096 + 4096 + 656), 5 buffers
  with the header. **Never overflows.**
- `ndescs = gso_segs` is up to 45 for a 64K skb at MSS 1448, against the ring; fine.

Small sends (many iovec entries, or many small `sendmsg()` calls that Nagle/cork/MSG_MORE merge
into the tail skb): every piece is its own frag.

- Non-GSO overflow: 16 frags x 90 bytes = 1440 bytes <= MSS 1448: `nr_frags` 16 > 15 ->
  linearize -> fails -> drop. Extreme: 17 frags x 1 byte = a 17-byte payload, dropped.
  Threshold: mean piece size below MSS/16 (about 90 bytes at MSS 1448, about 559 at MSS 8948).
  15 frags is fine (nsge 15).
- GSO overflow: frags `15 x 96 B` then one `4096 B`. skb is 5536 bytes of payload, GSO. First
  segment = header + 15 small frags (1440 B) + 8 B of the big frag = 17 buffers > 16 -> linearize
  the whole skb -> fails -> the whole 5.5K skb is dropped. A later segment overflows only if it
  touches 17 chunks, e.g. the tail of one big frag followed by 16 tiny ones.
- Either way the drop repeats on every retransmit until TCP gives up: a self-inflicted stall of
  that one connection, no memory-safety problem, no effect on other flows.

With `CONFIG_MAX_SKB_FRAGS` raised (up to 45) the window grows (16..45 frags), same mechanism.

### 2.5 Mitigation: decision

**Keep the existing behaviour (failed linearize -> drop), make it explicit and countable.**

- `ndo_features_check` clearing `NETIF_F_GSO_MASK` for such skbs does not help: the overflow needs
  16+ frag chunks inside one MSS, so the software-segmented packet has the same 16+ frags and
  hits the non-GSO limit. Clearing `NETIF_F_SG` just moves the same failed linearize into the
  core. There is no per-packet frag-count knob in the stack.
- `netif_set_tso_max_segs()` / `netif_set_tso_max_size()` limit segments and bytes per GSO skb, not
  buffers per segment; irrelevant to this limit.
- Splitting one MSS-sized packet across descriptors is not possible in the ionic descriptor format
  (one descriptor = one packet or one TSO segment, 4-bit NSGE).
- Precedent: gve's netmem TX (c32532670cec, by the devmem maintainer) leaves its
  `skb_linearize()` fallback as is and drops on failure
  (`74:drivers/net/ethernet/google/gve/gve_tx_dqo.c:1018-1021`).

Suggested shape for the patch (small, what reviewers are likely to accept):

```c
linearize:
	if (too_many_frags) {
		err = skb_linearize(skb);	/* fails for unreadable frags */
		if (unlikely(err))
			return err;
		q_to_tx_stats(q)->linearize++;
	}
```
left as is, plus at most a comment and (optional, PoC-friendly) a per-queue ethtool counter for
the drop, since `q->drop` is debugfs-only. An explicit `if (!skb_frags_readable(skb)) return
-EOPNOTSUPP;` before `skb_linearize()` is equivalent and self-documenting but not required.
Upstream reviewers will most likely want: netmem unmap helpers, the `netmem_tx` flag, no new
`features_check`, and possibly the v0 `max_sg_elems` fix as a separate `Fixes:` patch.

---

## 3. Other payload touches on TX

| Site (`74:ionic_txrx.c`, `726:` line if different) | What it does | net_iov safe? | Change |
| --- | --- | --- | --- |
| `ionic_start_xmit()` 1820-1864 (`726:1813-1857`) | reads `skb_shinfo(skb)->tx_flags`, queue mapping; calls descs_needed, tso/tx | yes, metadata only | none |
| `ionic_start_hwstamp_xmit()` 1783-1818 (`726:1776-1811`) | same on `lif->hwstamp_txq`; sets `SKBTX_HW_TSTAMP` | yes | none (same map/unmap code, same `q->dev`) |
| `ionic_tx_descs_needed()` 1695-1781 (`726:1688-1774`) | `skb_frag_size()` only; `skb_linearize()` fallback | linearize fails cleanly (`-ENOMEM`) | optional comment/counter (2.5) |
| Short-frame padding | **none**: no `skb_put_padto`/`eth_skb_pad`/`skb_pad` in the driver (grep `74:` and `726:`) | n/a | none. INFERRED: hardware pads runts; check the emulator pads (a 1-byte TCP payload without timestamps is a 55-byte frame) |
| `ionic_tx_tcp_pseudo_csum()` 1378-1397, `ionic_tx_tcp_inner_pseudo_csum()` 1354-1376 | `skb_cow_head(skb, 0)`, then writes `ip_hdr()->check`, `tcp_hdr()->check` in the linear header | yes. If the header is cloned, `pskb_expand_head()` only re-refs frags: `skb_orphan_frags()` returns 0 because msg_zerocopy uargs set `SKBFL_DONT_ORPHAN` (`74:net/core/skbuff.c:1750`, `74:include/linux/skbuff.h` `skb_orphan_frags()`) | none |
| Encapsulation | `skb->encapsulation` -> ENCAP flag, `skb_inner_tcp_all_headers()` for hdrlen (1463, 1489-1492, 1570-1573) | yes, header offsets only | none |
| `ionic_tx_calc_csum()` 1558-1595 / `ionic_tx_calc_no_csum()` 1597-1631 | descriptor from `buf_info->dma_addr`, `nr_frags`, `skb_checksum_start_offset()`, `skb->csum_offset`; device computes the checksum | yes, no software checksum anywhere in the driver | none |
| `ionic_tx_skb_frags()` 1633-1648 | copies `buf_info->dma_addr/len` into SG elems | yes | none |
| `ionic_tx()` 1650-1693 (`726:1650-1686`, tail differs) | map, descriptor, `skb_tx_timestamp()`, `stats->bytes += skb->len`, BQL | yes | none |
| `ionic_tx_tso()` 1435-1556 | section 2.3; `skb->len`, `gso_size`, `gso_type` | yes | none |
| `skb_tx_timestamp()` 1426, 1671 and `skb_tstamp_tx()` in `ionic_tx_clean()` 1223 | core: without `SOF_TIMESTAMPING_OPT_TSONLY` it clones and calls `skb_orphan_frags_rx()` (`74:net/core/skbuff.c:5855-5859`) -> `skb_copy_ubufs()` -> `-EFAULT` for unreadable (`:2010`), clone freed, no timestamp delivered | safe (no crash); timestamps for devmem skbs need OPT_TSONLY | none |
| `ionic_tx_map_skb()` 1120-1163 | `dma_map_single(skb->data, headlen)`; `skb_frag_dma_map()` per frag; **error unwind uses `dma_unmap_page()` on frags** (1154-1159) | map: yes. unwind: **no** | make unwind netmem-aware (3.3) |
| `ionic_tx_desc_unmap_bufs()` 1165-1183 | `dma_unmap_single()` buf 0, **`dma_unmap_page()` bufs 1..n** | **no** | make netmem-aware (3.3) |
| `ionic_tx_clean()` 1185-1236 | unmap; hwstamp; `desc_info->bytes = skb->len`; `napi_consume_skb()` | yes apart from the unmap; frag release is core | none beyond unmap |
| `ionic_tx_service()` 1238-1282 / `ionic_tx_cq_service()` 1284-1315 | completion walk, BQL with `desc_info->bytes` | yes | none |
| `ionic_tx_flush()` 1317-1325 + `ionic_tx_empty()` 1327-1352, called from `ionic_txrx_deinit()` (`74:ionic_lif.c:2016-2046`, `726:2013-`) for each txq and the hwstamp txq | drains completions, then `ionic_tx_clean(q, desc_info, NULL, false)` for every in-flight descriptor | same unmap function, so fixed by the same change | none beyond unmap |
| `ionic_qcq_free()` (`74:ionic_lif.c:417-461`) | frees rings and `vfree(q.info)`; no per-buffer unmap | n/a | none |
| XDP: `ionic_xdp_post_frame()` 304-395, `ionic_xdp_xmit()` 397-456, `ionic_run_xdp()` XDP_TX 547-584, `ionic_xdp_tx_desc_clean()` 274-302 | XDP_TX uses page_pool DMA addresses, no unmap. **XDP_REDIRECT shares `ionic_tx_map_single/_frag()` and `ionic_tx_desc_unmap_bufs()`** with skb TX (calls at 292 and 365) and shares the txq ring slots with skbs | XDP frames are always page-backed | the unmap change must keep unmapping XDP_REDIRECT frags; see marker choice in 3.3 |
| `ionic_write_cmb_desc()` 766-774 | copies the 16-byte descriptor to the CMB | yes | none |
| Stats: `ionic_stats.c`, `ionic_get_stats64()` | no skb access at all (grep: no `skb` in `ionic_stats.c`, `ionic_phc.c`, `ionic_main.c`, `ionic_ethtool.c`, `ionic_dev.c`, `ionic_lif.c`) | yes | none |

`ionic_ethtool.c` v7.2.6 -> v7.4-rc3: `assign_bit()` cleanup and `ionic_get_ts_info()` gating
hardware timestamping on `IONIC_ETH_HW_TIMESTAMP`; `ionic_phc.c`: RDMA PHC state page;
`ionic_main.c`: `port_info` free fixes. None touch TX data (VERIFIED from the diff).

### 3.3 Unmap inventory and design

Every `dma_unmap_*` reachable for an skb frag (VERIFIED, `74:ionic_txrx.c`, same lines at `726:`):

| # | Line | Call | Reached from |
| --- | --- | --- | --- |
| 1 | 1157-1158 | `dma_unmap_page(dev, buf_info->dma_addr, buf_info->len, DMA_TO_DEVICE)` | `ionic_tx_map_skb()` `dma_fail:` unwind of already-mapped frags (from `ionic_tx()` 1657 and `ionic_tx_tso()` 1478) |
| 2 | 1160-1161 | `dma_unmap_single(...bufs[0]...)` | same unwind, head (always kernel memory, correct as is) |
| 3 | 1175-1176 | `dma_unmap_single(...)` buf 0 | `ionic_tx_desc_unmap_bufs()` |
| 4 | 1179-1180 | `dma_unmap_page(...)` bufs 1..nbufs-1 | `ionic_tx_desc_unmap_bufs()` |

Callers of `ionic_tx_desc_unmap_bufs()`: `ionic_tx_clean()` 1203 (completion via
`ionic_tx_service()` 1270; teardown via `ionic_tx_empty()` 1338), `ionic_xdp_tx_desc_clean()` 292
(XDP_REDIRECT completion), `ionic_xdp_post_frame()` 365 (XDP_REDIRECT map-failure unwind).

Per-buffer state recorded (`74:ionic_dev.h:206-223`, identical at `726:`):

```c
struct ionic_buf_info {
	struct page *page;
	dma_addr_t dma_addr;
	u32 page_offset;
	u32 len;
};
struct ionic_tx_desc_info {
	unsigned int bytes;
	unsigned int nbufs;
	struct sk_buff *skb;
	struct xdp_frame *xdpf;
	enum xdp_action act;
	struct ionic_buf_info bufs[MAX_SKB_FRAGS + 1];
};
```
- skb path (`ionic_tx_map_skb()`): sets only `dma_addr` and `len` for bufs 0..nr_frags, and
  `nbufs = 1 + nr_frags`. **`page` and `page_offset` are not written, so they hold stale values**
  from an earlier XDP use of the same ring slot.
- XDP path: sets `dma_addr`, `len`, `page` (`buf_info->page = page` at 334, `bi->page =
  skb_frag_page(frag)` at 371), `page_offset` for buf 0.
- `dma_addr` is read after mapping only to build descriptors in the same xmit call
  (`ionic_tx_calc_*`, `ionic_tx_skb_frags()`, `ionic_tx_tso()`) and later for unmap.
- For TSO all buffers hang off the first descriptor; later ones have `nbufs = 0`.

Why unmapping a net_iov address is wrong: netmem.rst TX requirement 1, "The Driver must not pass
the netmem dma_addr to any of the dma-mapping APIs directly ... Helpers like
netmem_dma_unmap_page_attrs() & netmem_dma_unmap_addr_set() should be used"
(`74:Documentation/networking/netmem.rst:136-145`). The helpers are identical in both kernels
(`74:include/net/netmem.h:447-464`, `7215:include/net/netmem.h:397-414`):
`netmem_dma_unmap_addr_set()` stores 0 for a net_iov, `netmem_dma_unmap_page_attrs()` returns
early on `addr == 0`.

Design options:

- **A (recommended, zero struct growth, no XDP change)**: record the frag's page in the slot that
  the skb path currently leaves stale, exactly as the XDP path already does, and skip the unmap
  when it is NULL. `skb_frag_page()` returns NULL for a net_iov frag in both kernels
  (`74:include/linux/skbuff.h:3727-3733`, `7215:include/linux/skbuff.h:3718-3724`).
  - `ionic_tx_map_skb()` frag loop: add `buf_info->page = skb_frag_page(frag);`
  - `ionic_tx_map_skb()` unwind (site 1) and `ionic_tx_desc_unmap_bufs()` (site 4): unmap through
    `netmem_dma_unmap_page_attrs(dev, buf_info->page ? buf_info->dma_addr : 0, buf_info->len,
    DMA_TO_DEVICE, 0)` (or a plain `if (buf_info->page)`).
  - XDP_REDIRECT frags already have a non-NULL `page` (line 371), so they keep being unmapped.
    The head buffer (index 0) stays `dma_unmap_single()` unconditionally.
  - Must be written for every frag on the skb path because the slot is reused across XDP and skb
    users of the same txq.
- **B (the idiom reviewers know from mlx5/bnxt/gve)**: add a `DEFINE_DMA_UNMAP_ADDR(unmap_addr)`
  style field to `ionic_buf_info`, set it with `netmem_dma_unmap_addr_set(skb_frag_netmem(frag),
  buf_info, unmap_addr, dma_addr)` in `ionic_tx_map_skb()` and with the plain address in
  `ionic_xdp_post_frame()` (lines 332 and 369), and unmap from it. Costs 8 bytes x 18 buffers per
  TX descriptor (and per RX buffer, the struct is shared), and touches the XDP path.
- Not recommended: zeroing `dma_addr` for net_iov (TSO and SG fill read it), or deciding from
  `desc_info->skb` at unmap time (XDP callers have no skb; per-frag state is more robust).

No DMA sync is needed for TX of dmabuf memory (`DMA_TO_DEVICE`, never bounced; INFERRED from
netmem.rst item 6 wording about delegating sync, not TX-specific).

---

## 4. Firmware / device assumptions about buffer addresses

- VERIFIED: 52-bit addressing. `/* the device's internal addressing uses up to 52 bits */
  #define IONIC_ADDR_LEN 52` (`74:ionic_if.h:813-815`, `726:ionic_if.h:784-786`); DMA mask set to
  match: `dma_set_mask_and_coherent(dev, DMA_BIT_MASK(IONIC_ADDR_LEN))`
  (`74:ionic_bus_pci.c:325`).
- VERIFIED: the main descriptor packs the address into `cmd` bits 12..63 and **masks silently**:
  `cmd |= (addr & IONIC_TXQ_DESC_ADDR_MASK) << IONIC_TXQ_DESC_ADDR_SHIFT;` with
  `IONIC_TXQ_DESC_ADDR_MASK (BIT_ULL(IONIC_ADDR_LEN) - 1)`, shift 12
  (`74:ionic_if.h:946-947,978-989`; `726:ionic_if.h:917-918,949-960`). SG elements carry a full
  `__le64 addr` (`74:ionic_if.h:1006-1010`).
- VERIFIED: where devmem addresses land. Non-TSO: the head (kernel memory) is in `cmd`, devmem
  frags only in SG elems. **TSO: a segment that starts inside a frag puts the devmem address in
  `cmd`** (`desc_addr = frag_addr`, `74:ionic_txrx.c:1524`), so it goes through the 52-bit mask.
- INFERRED: the dmabuf is mapped by the core for the netdev's parent device
  (`netdev_queue_get_dma_dev()` -> `dev->dev.parent`, `74:net/core/netdev_queues.c:10-21`;
  ionic does `SET_NETDEV_DEV(netdev, dev)` at `74:ionic_lif.c:3256` with the same `ionic->dev`
  used as `q->dev` at `:580`), so the attachment honours the 52-bit mask as long as the exporter
  maps through the DMA API. An exporter handing out raw bus addresses above 2^52 would be
  truncated without any check; the driver has no range check anywhere.
- VERIFIED: lengths are 16-bit: `desc->len` and `elem->len` are `__le16`
  (`74:ionic_if.h:961,1008`), `#define IONIC_MAX_BUF_LEN ((u16)-1)` (`74:ionic_dev.h:198`).
  Non-TSO chunks are bounded by the MTU, TSO chunks by `min(frag_rem, seg_rem)` <= hdrlen + mss.
  The driver does not check a frag length against 65535 on TX; with 64K pages and a jumbo-less
  MTU this still cannot exceed it for a single packet (INFERRED).
- VERIFIED: no alignment or minimum-length requirement for TX data buffers or SG elements is
  stated in `ionic_if.h` (grep for align/minimum: only ring-size, CMB 4K alignment, and
  `eth.min_frame_size`/`max_frame_size`, which the driver uses solely for MTU bounds,
  `74:ionic_lif.c:3276-3279`). `ionic_tx_tso()` freely emits 1-byte chunks at arbitrary offsets.
  Real firmware tolerance of many tiny SG elements is UNVERIFIABLE from the driver.
- VERIFIED: rings must be DMA-contiguous and page-aligned (`PTR_ALIGN(..., PAGE_SIZE)`,
  `74:ionic_lif.c:700-726`); irrelevant to data buffers.
- VERIFIED: TSO contract, `74:ionic_if.h:880-900`: driver zeroes the IPv4 checksum and preloads
  the TCP checksum with the pseudo header at length 0; `hdr_len` includes the L4 header; the
  header template is "the first data buffer". The emulator must honour this if TSO is ever
  granted.
- Emulator view (from `ernic-emu-datapath.md`, not re-verified here): reads `addr`, `nsge`, `len`
  only; DMA goes through `vfu_addr_to_sgl()`, so an address outside a registered, fd-backed region
  fails and the frame is dropped.

---

## 5. Patch touch points, line numbers, backport

### 5.1 Functions a devmem TX patch must or may touch

| File | Function / item | v7.4-rc3 | v7.2.6 | Change | Same hunk applies to v7.2.6? |
| --- | --- | --- | --- | --- | --- |
| `ionic_lif.c` | `ionic_init_nic_features()` | 1574-1655 (insert after 1642) | 1571-1652 (after 1639) | set `netdev->netmem_tx` when SG+CSUM granted | yes, offset -3, context identical |
| `ionic_lif.c` | `ionic_set_features()` | 1657-1669 | 1654-1666 | only for option 1.3(2b) | yes, offset -3 |
| `ionic_lif.c` | `ionic_netdev_ops` | 2821-2846 | 2818-2843 | none planned (no features_check) | n/a |
| `ionic_lif.c` | `ionic_lif_queue_identify()` override | 3909-3993 (3989) | 3906-3990 (3986) | optional separate fix: honour v0 SG size | yes, offset -3 |
| `ionic_txrx.c` | `ionic_tx_map_frag()` | 1103-1118 | same | none (already net_iov aware via `skb_frag_dma_map`) | n/a |
| `ionic_txrx.c` | `ionic_tx_map_skb()` | 1120-1163 | same | record marker per frag; netmem-aware unwind | yes, exact |
| `ionic_txrx.c` | `ionic_tx_desc_unmap_bufs()` | 1165-1183 | same | netmem-aware frag unmap | yes, exact |
| `ionic_txrx.c` | `ionic_xdp_post_frame()` | 304-395 | same | only with design B | yes, exact |
| `ionic_txrx.c` | `ionic_tx_descs_needed()` | 1695-1781 (linearize 1772-1778) | 1688-1774 (1765-1771) | optional comment/counter | yes, offset -7 |
| `ionic_txrx.c` | `ionic_start_xmit()` / `ionic_start_hwstamp_xmit()` | 1820 / 1783 | 1813 / 1776 | none (optional counter at `err_out_drop`) | yes, offset -7 |
| `ionic_txrx.c` | `ionic_tx()` | 1650-1693 | 1650-1686 | **avoid**: the only function whose body differs | a hunk in lines 1672-1692 would need a hand backport |
| `ionic_txrx.c` | `#include <net/netmem.h>` | top, after line 9 | same | for the helpers (design A with the helper, or B) | yes |
| `ionic_dev.h` | `struct ionic_buf_info` / `ionic_tx_desc_info` | 206-211 / 216-223 | same | only with design B | yes, file identical |
| `ionic_lif.h` / `ionic_stats.c` | `struct ionic_tx_stats` (26-, `linearize` at 36) / `IONIC_TX_STAT_DESC` list (131-144) | same | same | optional new counter | yes |

Differences v7.2.6 -> v7.4-rc3 that matter (VERIFIED, full `git diff v7.2.6 v7.4-rc3 --
drivers/net/ethernet/pensando/ionic`, 10 files, +150/-57):

- `ionic_txrx.c`: one hunk, in `ionic_tx()` at 1672 (c91b4d6e5cc3 "ionic: use
  netif_txq_maybe_stop() in ionic_tx()"): posts before the stop check and rings the doorbell
  by hand. Everything above line 1672 has identical line numbers; everything below is +7.
  **v7.2.15's `ionic_txrx.c` is identical to v7.4-rc3's** (`git diff v7.2.15 v7.4-rc3` empty), so
  the stable tree picked this fix up; the v7.2.6 DKMS pin lacks it.
- `ionic_lif.c`: `devm_kzalloc_obj*` conversions (line-neutral), the PHC feature-bit change and
  one extra `dev_dbg` in `ionic_set_nic_features()` (+3 lines from 1513).
- `ionic_if.h`: +29/+30 lines of RDMA/SRQ identity fields before the TXQ definitions, and
  `IONIC_ETH_HW_RDMA_TIMESTAMP = BIT(21)`; TXQ descriptor definitions unchanged
  (`IONIC_ADDR_LEN` at 814 vs 785, `encode_txq_desc_cmd()` at 978 vs 949).
- `ionic_dev.h`, `ionic_dev.c`, `ionic_stats.c`: no change.

Verdict: **one patch series serves both**, provided it does not edit `ionic_tx()`'s tail. `git am`
/ `patch` resolve the -3 and -7 offsets; no hand backport. Core API the patch uses
(`netmem_tx` enum, `NETMEM_TX_DMA`, `netmem_dma_unmap_page_attrs()`, `skb_frag_page()` returning
NULL for net_iov, `skb_frag_dma_map()` handling net_iov) is present with the same signatures in
v7.2.15 (lines cited above). The one internal difference is `__skb_frag_dma_map()`: 7.4 decodes
with `netmem_dma_addr_decode(netmem_get_dma_addr(...))` (`74:include/linux/skbuff.h:3830-3834`),
7.2.15 reads `netmem_to_net_iov(frag->netmem)->desc.dma_addr`
(`7215:include/linux/skbuff.h:3819-3822`); both are inline in the header the DKMS build uses, so
no driver-visible difference.

### 5.2 rocm-ernic patches on top of v7.2.6

From `/var/tmp/ionic-devmem/rocm-ernic-pin/ansible/files/patches/` (VERIFIED from each patch's
`+++` lines):

| Patch | Ethernet-driver files touched |
| --- | --- |
| `0002-ionic-allocate-an-address-handle-for-UC-queue-pairs.patch` (top level) | none (`drivers/infiniband/hw/ionic/ionic_controlpath.c` only) |
| `srq/0001`, `0002`, `0003`, `0005`-`0009` | none (`drivers/infiniband/hw/ionic/*`, `include/uapi/rdma/ionic-abi.h`) |
| `srq/0004-net-ionic-Fetch-qid-allocation-and-SRQ-capability-fr.patch` | `ionic_if.h` only (hunks at 7, 513, 555, 600, 2603, 2632: the same identity/RDMA additions that are in v7.4-rc3) |
| `p4/0001-ionic-register-a-P4-auxiliary-device.patch` | `ionic_aux.c`, `ionic_if.h` (hunk at 363), `ionic_lif.h` (one line in `struct ionic_lif` at 226) |
| `rdma-core/*` | userspace rdma-core, not kernel |

**None touches `ionic_txrx.c`, `ionic_lif.c`, or `ionic_dev.h`.** The `ionic_if.h` hunks sit above
the TXQ definitions (shifting them down, closer to the v7.4-rc3 numbering) and the `ionic_lif.h`
hunk is far below `struct ionic_tx_stats`, so a devmem patch adding a TX stat or using the TXQ
macros applies after them with offsets only.

---

## 6. Open questions

1. Emulator prerequisites (not driver work): grant TX_SG + TX_CSUM, implement CSUM_PARTIAL, and
   identify TXQ v1 with 256-byte SG descriptors; otherwise the driver emits nsge up to 15 against
   an 8-element v0 slot. Alternatively fix the driver's `max_sg_elems` override.
2. Should the PoC close the "user turns SG and HW_CSUM both off" zero-fill case in-driver
   (toggle `netmem_tx` in `ionic_set_features()`), or leave it as a core issue to raise on netdev?
3. Is a visible counter for dropped unreadable skbs wanted (new ethtool stat) or is debugfs
   `q->drop` enough for the PoC?
4. Does real ionic firmware pad runt frames and accept many 1-byte SG elements? The driver gives
   no evidence either way; only matters for hardware, not rocm-ernic.
5. `unreadable` propagation through software GSO (`skb_segment()`) and TSO-off operation were
   reasoned from source, not run.
6. Whether a dmabuf exporter used in the test (udmabuf vs a GPU exporter) can produce DMA
   addresses above 2^52 behind the guest's (non-)IOMMU; the driver would truncate silently in
   the TSO path.

---

## Verification pass

Independent re-read of the source on 2026-11-18 by a second reviewer. Nothing was built or run.
Same tree prefixes as above (`74:`, `726:`, `7215:`). Each claim is CONFIRMED, WRONG (with the
correction) or UNVERIFIABLE. Nothing above this heading was edited.

### Corrections (these change the summary)

- **V1. WRONG: the on-wire result of the "SG off + HW_CSUM off" GSO row (section 0 item 5,
  table row 5 in 1.2).** The notes infer "bad L4 checksum, receiver drops". The segments carry a
  **valid** TCP checksum over the zero-filled payload, so the receiver accepts zeros as data.
  `skb_copy_and_csum_bits()` zero-fills and returns 0 (`74:net/core/skbuff.c:3659-3664`,
  `7215:net/core/skbuff.c:3636-3641`), and 0 is the correct partial sum of an all-zero buffer;
  `skb_segment()` stores it in `SKB_GSO_CB(nskb)->csum` with `ip_summed = CHECKSUM_NONE`
  (`74:net/core/skbuff.c:5068-5078`, `7215::4982-4992`); `tcp_gso_segment()` then builds the
  final checksum from it, `th->check = gso_make_checksum(skb, ~th->check)`
  (`74:net/ipv4/tcp_offload.c:212-215`, `74:include/net/gso.h:75-85`). The sentence quoted from
  ab9414ed70bd ("checksum usually won't match") is about its other callers (ICMP quoting,
  `skb_copy_and_csum_dev()`), where the checksum was computed elsewhere. Consequence: this state
  is **silent payload corruption delivered to the peer application**, not a stall. Derived from
  source, not run. It makes gating `netmem_tx` on the grant mandatory rather than nice to have,
  and makes "do nothing at runtime, like mlx5/bnxt" (1.3 option 2a) a weaker choice: a user
  running `ethtool -K <dev> sg off tx off` on a bound device turns large devmem sends into
  zeros. No GSO-path check stops it (no `skb_frags_readable()` test in `net/core/gso.c`,
  `net/ipv4/tcp_offload.c`, or `skb_segment()` at either tag).
  Extra observation (INFERRED): those segments are linear but still have `skb->unreadable` set,
  because `__copy_skb_header()` copies the header group (`74:net/core/skbuff.c:1570`,
  `74:include/linux/skbuff.h:976-1093`), so `ionic_start_xmit()` could drop
  `!skb_frags_readable(skb) && !skb_shinfo(skb)->nr_frags` as a driver-local backstop. No
  precedent in other drivers.
- **V2. WRONG (incomplete): "the driver forces `max_sg_elems` = 15" (section 0 item 4, 2.1).**
  True only when Q_IDENTIFY for the TXQ succeeds. `qti` is zeroed first
  (`74:ionic_lif.c:3934`), and on `-EINVAL` the loop `continue`s, on any other error the
  function `return`s (`74:ionic_lif.c:3953-3963`, `726:` -3), both before the override at
  `74:ionic_lif.c:3989`. Then `max_sg_elems` stays 0, every skb with a frag takes the linearize
  path (`74:ionic_txrx.c:1720`), and **every devmem skb is dropped**. The emulator must keep
  answering Q_IDENTIFY for the TXQ (it does on develop per `ernic-emu-datapath.md`).
- **V3. WRONG (overstated): "sendmsg always succeeds whatever `dev->features` says" (1.2 first
  bullet).** `sk_route_caps |= NETIF_F_SG | NETIF_F_HW_CSUM` sits inside `if (sk_can_gso(sk))`
  and its `else` branch (`74:net/core/sock.c:2687-2691`, `7215:net/core/sock.c:2658-2662`). A
  socket with GSO disabled (TCP-MD5/TCP-AO keys call `sk_gso_disable()`,
  `74:net/ipv4/tcp_ipv4.c:1340`, `74:net/ipv4/tcp_ao.c:1722`) or a non-offloaded xfrm route
  keeps the device's bits, so with device SG off `zc != MSG_ZEROCOPY` and devmem `sendmsg()`
  returns `-EINVAL` (`74:net/ipv4/tcp.c:1159-1162,1178-1180`). For ordinary TCP sockets the
  claim holds.
- **V4. WRONG (minor): "mlx5, bnxt, gve, fbnic set `netmem_tx` unconditionally" (1.3 item 2).**
  gve sets it conditionally: `if (!gve_is_gqi(priv) && !gve_is_qpl(priv)) dev->netmem_tx =
  NETMEM_TX_DMA;` (`74:drivers/net/ethernet/google/gve/gve_main.c:3079-3080`,
  `7215:...gve_main.c:2897`). That is direct precedent for the conditional set proposed for
  ionic. None of the four touches `netmem_tx` on a feature toggle (grep, `74:`), so that half
  is CONFIRMED.
- **V5. Addition, not in the notes:** after a firmware reset `ionic_lif_init()` calls
  `ionic_set_nic_features(lif, lif->netdev->features)` instead of `ionic_init_nic_features()`
  (`74:ionic_lif.c:3785-3788`). `lif->hw_features` is recomputed but `netdev->features`, and a
  `netmem_tx` set in `ionic_init_nic_features()`, are not. A device granting less after reset
  leaves `netmem_tx` set. Low risk for the PoC.

### Confirmed

- **Map side is net_iov-aware, unmap is not.** `__skb_frag_dma_map()` branches on
  `skb_frag_is_net_iov()` (`74:include/linux/skbuff.h:3823-3837`,
  `7215:include/linux/skbuff.h:3814-3825`). Frag unmaps are `dma_unmap_page()` at
  `74:ionic_txrx.c:1157` and `:1179`; head unmaps `dma_unmap_single()` at `:1160`, `:1175`.
  Callers of `ionic_tx_desc_unmap_bufs()`: `:292`, `:365`, `:1203`; `ionic_tx_clean()` from
  `:1270` and `:1338`; `ionic_tx_flush()/ionic_tx_empty()` from `74:ionic_lif.c:2023-2024,
  2038-2039`. No other `dma_unmap_*` in the file. Same lines at `726:` (file identical above
  line 1672).
- **Design A is sound.** `skb_frag_page()` returns NULL for net_iov at both tags
  (`74:include/linux/skbuff.h:3727-3733`, `7215::3718-3724`). The skb path writes only
  `dma_addr`/`len` (`74:ionic_txrx.c:1133-1146`); XDP writes `page` for the head (`:334`) and
  every frag (`:371`); XDP_TX never reaches the unmap (`:287-293`). No other TX code reads
  `buf_info->page` (all other uses in `ionic_txrx.c` are RX, lines 121-257, 465-574, 831-863).
  The head buffer needs no marker. Tried to break it via the XDP_REDIRECT map-failure unwind
  (`:362-366`): `nbufs` counts only buffers whose `page` was already written, so it holds.
- **Features.** No `.ndo_fix_features`/`.ndo_features_check` (grep: only `.ndo_set_features` at
  `74:ionic_lif.c:2829`); no `tso_max`/`gso_max` in the driver; netdev bits come only from
  `lif->hw_features` (`74:ionic_lif.c:1604-1643`); `ionic_set_features()` only forwards
  (`:1657-1669`); TX consults only the skb (`74:ionic_txrx.c:1663`, `:1850`).
- **Feature-state table rows 1-4.** `validate_xmit_skb()` order and backstops
  (`74:net/core/dev.c:4169-4196,4202-4270`); `skb_checksum_help()` `-EFAULT`
  (`74::3679-3681`, `7215::3624-3626`); `__pskb_pull_tail()` NULL (`74:net/core/skbuff.c:2906`,
  `7215::2886`); `skb_copy_bits()` fault (`74::3062`); sg && !csum linearize of the segment
  (`74::5182-5186`) with `SKBFL_SHARED_FRAG` from `SKBFL_ZEROCOPY_FRAG` (`74::1750`). The
  software-GSO sg && csum path passes `skb_orphan_frags()` because of `SKBFL_DONT_ORPHAN`
  (`74:net/core/skbuff.c:4958`, `74:include/linux/skbuff.h:3430-3437`) and shares frags by
  reference. `netdev_fix_features()` has no "SG needs checksum" rule (`74:net/core/dev.c:
  11181-11222`); it does drop `NETIF_F_GSO` without SG (`:11218-11222`), which TCP overrides
  (`74:net/core/sock.c:2680-2683`).
- **Bind fails cleanly without `netmem_tx`** (`74:net/core/netdev-genl.c:1241-1246`,
  `7215::1201`). ionic needs no `queue_mgmt_ops`: the DMA device falls back to
  `dev->dev.parent` (`74:net/core/netdev_queues.c:15-20`, same at `7215:`), which is
  `ionic->dev` (`74:ionic_lif.c:3256`, `q.dev` at `:580`). `ionic_init_nic_features()` at
  `:3788` runs before `register_netdev()` at `:3880`. So the kernel side is driver-only.
- **Frag limit.** Override at `74:ionic_lif.c:3982-3989` (`726:3979-3986`) when Q_IDENTIFY
  succeeds (see V2); SG slot size honours the device (`:1004-1008`, `:2091-2096`,
  `:2989-2994`) and `ionic_tx_sg_elems()` picks the array by `sg_desc_size`
  (`74:ionic_txrx.c:105-111`), so the v0 overflow is real. `ionic_tx_tso()` has no SG bound
  (`:1511-1537`); the dry run mirrors it (`:1733-1770`). Linearize triggers: `nr_frags > 15`
  (`:1720`), or `desc_bufs > 16` (`:1754`). Failed linearize gives `q->drop++` and a free
  (`:1841-1843,1860-1863`; hwstamp `:1796-1798,1814-1817`); `q->drop` is debugfs-only
  (`74:ionic_debugfs.c:145`; not in `ionic_stats.c`; `tx_dropped` is firmware's,
  `74:ionic_lif.c:1314-1316`).
- **Worst cases.** Frag construction (`74:net/core/datagram.c:706-747`,
  `74:net/core/devmem.c:452-463`), append to an unreadable tail (`74:net/ipv4/tcp.c:1256-1257`),
  no collapse (`74:net/ipv4/tcp_output.c:3561`). Arithmetic rechecked: bulk page-sized frags
  need at most 4 (MSS 1448) or 5 (MSS 8948) buffers; 16 x 90 B trips the non-GSO test; 15 x
  96 B + 4096 B trips the GSO dry run at buffer 17.
- **gve precedent** (`74:drivers/net/ethernet/google/gve/gve_tx_dqo.c:1018-1021`).
- **Other payload touches.** No pad/copy/checksum call in the driver TX path (grep of
  `ionic_txrx.c`, `ionic_lif.c`, `ionic_dev.c`, `ionic_main.c`, `ionic_ethtool.c`,
  `ionic_stats.c`, `ionic_phc.c`: only `skb_cow_head()` at `:1358,1382` and `skb_linearize()`
  at `:1774`). `pskb_expand_head()` on a cloned skb goes through `skb_orphan_frags()`
  (`74:net/core/skbuff.c:2349-2353`), a no-op with `SKBFL_DONT_ORPHAN`. Timestamp clone path
  fails closed (`74:net/core/skbuff.c:5855-5859`, `:2010-2011`).
- **Addresses.** `IONIC_ADDR_LEN 52` (`74:ionic_if.h:814`), DMA mask
  (`74:ionic_bus_pci.c:325`), silent mask in `encode_txq_desc_cmd()` (`74:ionic_if.h:978-989`),
  TSO puts a frag address in `cmd` (`74:ionic_txrx.c:1524`), `__le16` lengths.
- **Backport.** `git diff v7.2.6 v7.4-rc3` on `ionic_txrx.c` is the single `ionic_tx()` hunk
  at 1672 (+7); `ionic_dev.h`, `ionic_dev.c`, `ionic_stats.c` unchanged; `ionic_lif.c` hunks at
  570, 792, 1513 (+1), 1564 (+2), 2956, 2967; `ionic_lif.h` changes only `struct ionic_phc`.
  `git diff v7.2.15 v7.4-rc3` has no `ionic_txrx.c` hunk. Function start lines match table 5.1
  at both tags. `netmem_tx`, `NETMEM_TX_DMA`, the netmem unmap helpers exist in `7215:` with
  the same signatures (`7215:include/linux/netdevice.h:1811,2166`,
  `7215:include/net/netmem.h:397-414`).
- **rocm-ernic patches.** From every patch's `+++` lines: only `srq/0004` (`ionic_if.h`) and
  `p4/0001` (`ionic_aux.c`, `ionic_if.h`, `ionic_lif.h`) touch
  `drivers/net/ethernet/pensando/ionic/`.

### Unverifiable from source

- Real firmware behaviour for runt frames, tiny SG elements, and addresses above 2^52.
- That `unreadable` segments from software GSO transmit correctly end to end (source says yes;
  not run).
- Whether upstream reviewers would accept design A's reuse of `buf_info->page` over the
  `netmem_dma_unmap_addr_set()` idiom, and whether they would insist on closing V1 in the core
  first.
