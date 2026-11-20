# ionic Ethernet TX: the device-side contract an emulated NIC must implement

Read-only research, 2026-11-18. Nothing in any tree was modified.

Sources and citation form:

- `K74:` = `/var/tmp/ionic-devmem/linux` at **v7.4-rc3**, paths under
  `drivers/net/ethernet/pensando/ionic/` unless a full path is given.
- `K726:` = `/var/tmp/ionic-devmem/linux-7.2.6` at **v7.2.6** (DKMS ionic source pin), same directory.
- `C7215:` = `/var/tmp/ionic-devmem/linux-7.2.15` at **v7.2.15** (core kernel the module runs on).
- `E:` = rocm-ernic `~/Projects/rocm-ernic` at `bd902345` (branch `feat/devmem-poc` == develop).

Every statement is marked **VERIFIED** (read in the cited source) or **INFERRED** (reasoned, not read or
not tested). Line numbers are for the tag named; where v7.2.6 differs the v7.2.6 number is given too.

## 0. v7.2.6 vs v7.4-rc3: what differs (VERIFIED, `git diff v7.2.6 v7.4-rc3 -- drivers/net/ethernet/pensando/ionic/`)

Nothing in the TX wire contract differs. The whole diff relevant to this note is:

| File | Change | Effect on device contract |
| --- | --- | --- |
| `ionic_if.h` | adds `IONIC_ETH_HW_RDMA_TIMESTAMP = BIT(21)`, RDMA `srq_qtype`/`alloc_qid_cap` identity fields, `udma_idx`, a CQ magic | none for Ethernet TX; all TX structs, opcodes, flags, feature bits 0..20 are byte-identical |
| `ionic_lif.c` | `devm_kcalloc` -> `devm_kzalloc_objs` spelling; `ionic_set_nic_features()` ORs in `ident.lif.eth.config.features & (TIMESTAMP\|RDMA_TIMESTAMP)` instead of unconditional `TIMESTAMP` when `lif->phc` | none unless the device advertises TIMESTAMP (it must not, section 1) |
| `ionic_txrx.c` | `ionic_tx()` tail: v7.2.6 calls `ionic_txq_post(q, ring_dbell)` after a `netif_tx_stop_queue()` test; v7.4-rc3 posts first, uses `netif_txq_maybe_stop()`, then rings the doorbell separately | doorbell timing only; same descriptor contents, same p_index semantics |

Line-number mapping used below:

- `ionic_txrx.c`: identical through line 1671. After `ionic_tx()`: v7.2.6 = v7.4-rc3 - 7.
- `ionic_lif.c`: identical through 1514; 1515..1565: v7.2.6 = v7.4-rc3 - 1; from `ionic_init_nic_features()`
  on: v7.2.6 = v7.4-rc3 - 3.
- `ionic_if.h`: lines 8..513: -1; TX descriptor region (lines 800..1200 at v7.4-rc3): **-29**;
  `lif_setattr` region: -30; `ionic_lif_stats`: -32.

---------------------------------------------------------------------------------------------------

## 1. (a) Feature negotiation

### 1.1 Feature bits (VERIFIED `K74:ionic_if.h:1191-1214`; `K726:ionic_if.h:1162-1184`)

```c
enum ionic_eth_hw_features {
	IONIC_ETH_HW_VLAN_TX_TAG	= BIT(0),
	IONIC_ETH_HW_VLAN_RX_STRIP	= BIT(1),
	IONIC_ETH_HW_VLAN_RX_FILTER	= BIT(2),
	IONIC_ETH_HW_RX_HASH		= BIT(3),
	IONIC_ETH_HW_RX_CSUM		= BIT(4),
	IONIC_ETH_HW_TX_SG		= BIT(5),
	IONIC_ETH_HW_RX_SG		= BIT(6),
	IONIC_ETH_HW_TX_CSUM		= BIT(7),
	IONIC_ETH_HW_TSO		= BIT(8),
	IONIC_ETH_HW_TSO_IPV6		= BIT(9),
	IONIC_ETH_HW_TSO_ECN		= BIT(10),
	IONIC_ETH_HW_TSO_GRE		= BIT(11),
	IONIC_ETH_HW_TSO_GRE_CSUM	= BIT(12),
	IONIC_ETH_HW_TSO_IPXIP4		= BIT(13),
	IONIC_ETH_HW_TSO_IPXIP6		= BIT(14),
	IONIC_ETH_HW_TSO_UDP		= BIT(15),
	IONIC_ETH_HW_TSO_UDP_CSUM	= BIT(16),
	IONIC_ETH_HW_RX_CSUM_GENEVE	= BIT(17),
	IONIC_ETH_HW_TX_CSUM_GENEVE	= BIT(18),
	IONIC_ETH_HW_TSO_GENEVE		= BIT(19),
	IONIC_ETH_HW_TIMESTAMP		= BIT(20),
	IONIC_ETH_HW_RDMA_TIMESTAMP	= BIT(21),   /* v7.4-rc3 only */
};
```

### 1.2 LIF_IDENTIFY `eth.config.features`

Layout (VERIFIED `K74:ionic_if.h:493-505,585-602`; offsets recomputed by hand and equal to the emulator's
macros at `E:src/ionic/ionic_eth_emu.c:1133-1143`):

```
union ionic_lif_identity (packed), devcmd data window
0x000 le64 capabilities
0x008 u8   eth.version            0x00c le32 max_ucast_filters   0x010 le32 max_mcast_filters
0x014 le16 rss_ind_tbl_sz         0x016 le32 min_frame_size      0x01a le32 max_frame_size
0x020 le64 hwstamp_tx_modes       0x028 le64 hwstamp_rx_filters  0x030 u8 rsvd3[88]
0x088 union ionic_lif_config:  +0 u8 state, +4 char name[16], +0x14 le32 mtu, +0x18 u8 mac[6],
                               +0x1e le16 vlan, +0x20 le64 features (= 0x0a8), +0x28 le32 queue_count[16]
```

What the driver does with `eth.config.features` (VERIFIED, whole-directory grep at v7.4-rc3):

- `K74:ionic_phc.c:582-584`: PHC is only set up if it has TIMESTAMP/RDMA_TIMESTAMP.
- `K74:ionic_lif.c:1515-1517`: ANDed with the two TIMESTAMP bits into the SETATTR request when `lif->phc`.
- `K74:ionic_lif.c:4019-4020`: `dev_dbg`.

So for SG/CSUM/TSO the identity field is **informational**: the driver neither requires nor trusts it.
The emulator should still set it to the mask it will grant (truthful identity), and must keep bits 20/21
clear (it has no hwstamp registers; `E:...:1142` leaves the field 0 today).

### 1.3 LIF_SETATTR(IONIC_LIF_ATTR_FEATURES)

Sent on the **adminq** (`ionic_adminq_post_wait`), at LIF init and on every `ethtool -K`
(VERIFIED `K74:ionic_lif.c:1495-1525,1657-1669,3786-3788`).

Command, 64 bytes (VERIFIED `K74:ionic_if.h:1915-1935`, `ionic_dev.h:98`):

```
0  u8   opcode = IONIC_CMD_LIF_SETATTR
1  u8   attr   = IONIC_LIF_ATTR_FEATURES (4)      (enum at K74:ionic_if.h:1884-1894)
2  le16 index  (LIF index)
4  le64 features      requested mask, enum ionic_eth_hw_features
12 ...  rest of the 60-byte union, zero
```

Completion, 16 bytes (VERIFIED `K74:ionic_if.h:1946-1955`, `ionic_dev.h:99`):

```
0  u8   status
1  u8   rsvd
2  le16 comp_index
4  le64 features      what the device actually enabled   (packed union with rsvd2[11])
15 u8   color
```

Driver side (VERIFIED `K74:ionic_lif.c:1513-1525`; `K726:ionic_lif.c:1513-1524`):

```c
	ctx.cmd.lif_setattr.features = ionic_netdev_features_to_nic(features);
	...
	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		return err;

	old_hw_features = lif->hw_features;
	lif->hw_features = le64_to_cpu(ctx.cmd.lif_setattr.features &
				       ctx.comp.lif_setattr.features);
```

Device contract: return `status = 0` and `features = requested & supported`. Extra bits in the answer
are harmless (the driver ANDs), a non-zero status fails `ionic_lif_init()`.

What the driver requests at init (VERIFIED `K74:ionic_lif.c:1581-1598`): VLAN_CTAG_TX/RX/FILTER, SG,
HW_CSUM, RXCSUM, TSO, TSO6, TSO_ECN, GSO_GRE, GSO_GRE_CSUM, GSO_IPXIP4, GSO_IPXIP6, GSO_UDP_TUNNEL,
GSO_UDP_TUNNEL_CSUM, plus RXHASH when `nxqs > 1`; translated by `ionic_netdev_features_to_nic()`
(`K74:ionic_lif.c:1455-1493`) to bits 0,1,2,(3),4,5,7,8,9,10,11,12,13,14,15,16. It **never** requests
`IONIC_ETH_HW_RX_SG` (bit 6) or the GENEVE bits.

Mapping of granted bits to netdev features (VERIFIED `K74:ionic_lif.c:1604-1643`;
`K726:ionic_lif.c:1601-1640`):

```c
	netdev->features |= NETIF_F_HIGHDMA;
	if (lif->hw_features & IONIC_ETH_HW_VLAN_TX_TAG)   netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_TX;
	if (lif->hw_features & IONIC_ETH_HW_VLAN_RX_STRIP) netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_RX;
	if (lif->hw_features & IONIC_ETH_HW_VLAN_RX_FILTER)netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_FILTER;
	if (lif->hw_features & IONIC_ETH_HW_RX_HASH)       netdev->hw_features |= NETIF_F_RXHASH;
	if (lif->hw_features & IONIC_ETH_HW_TX_SG)         netdev->hw_features |= NETIF_F_SG;

	if (lif->hw_features & IONIC_ETH_HW_TX_CSUM)       netdev->hw_enc_features |= NETIF_F_HW_CSUM;
	if (lif->hw_features & IONIC_ETH_HW_RX_CSUM)       netdev->hw_enc_features |= NETIF_F_RXCSUM;
	if (lif->hw_features & IONIC_ETH_HW_TSO)           netdev->hw_enc_features |= NETIF_F_TSO;
	if (lif->hw_features & IONIC_ETH_HW_TSO_IPV6)      netdev->hw_enc_features |= NETIF_F_TSO6;
	if (lif->hw_features & IONIC_ETH_HW_TSO_ECN)       netdev->hw_enc_features |= NETIF_F_TSO_ECN;
	... GRE, GRE_CSUM, IPXIP4, IPXIP6, UDP_TUNNEL, UDP_TUNNEL_CSUM likewise ...

	netdev->hw_features |= netdev->hw_enc_features;
	netdev->features |= netdev->hw_features;
	netdev->vlan_features |= netdev->features & ~NETIF_F_VLAN_FEATURES;
```

Consequences:

- VERIFIED: `NETIF_F_HW_CSUM` (generic, any protocol, `csum_start`/`csum_offset`) is what TX_CSUM yields,
  not `NETIF_F_IP_CSUM|IPV6_CSUM`. It also lands in `hw_enc_features`, so **encapsulated** skbs with an
  inner CHECKSUM_PARTIAL reach the device too (ENCAP flag, section 3.3).
- VERIFIED: the driver has no `ndo_fix_features`/`ndo_features_check` and sets no `gso_max_*`/`tso_max_*`
  (grep of the directory finds none), so core defaults apply.
- VERIFIED: `ionic_set_features()` just re-sends SETATTR; `netdev->hw_features` is computed once at init.
  The emulator can stay stateless: each descriptor's opcode says what to do.

### 1.4 Which bits to grant

| Stage | Grant | Result in guest |
| --- | --- | --- |
| 1 | `TX_SG (0x20) \| TX_CSUM (0x80)` = **0xa0** | `NETIF_F_SG`, `NETIF_F_HW_CSUM` |
| 2 | add `TSO (0x100) \| TSO_IPV6 (0x200)`, optionally `TSO_ECN (0x400)` = **0x3a0 / 0x7a0** | `NETIF_F_TSO`, `TSO6`, (`TSO_ECN`) |

Do not grant: tunnel TSO bits 11..16 (outer-header rewrite per segment), VLAN bits 0..2 unless VLAN
insertion/strip is implemented, RX_HASH/RX_CSUM unless the RX completion is filled accordingly,
TIMESTAMP.

### 1.5 RX features we do not grant (VERIFIED)

- RX_CSUM not granted -> `NETIF_F_RXCSUM` clear -> `ionic_rx_clean()` ignores `csum_flags` and leaves the
  skb CHECKSUM_NONE; the guest stack verifies every checksum in software (`K74:ionic_txrx.c:684-691`).
  This is why a TX checksum the emulator gets wrong is caught by a second guest.
- RX_HASH not granted -> no `skb_set_hash()` (`K74:ionic_txrx.c:667-682`); RSS config is only pushed when
  the bit changes (`K74:ionic_lif.c:1527-1528`).
- VLAN bits not granted -> `dev_info_once("NIC is not supporting vlan offload, likely in SmartNIC mode")`
  (`K74:ionic_lif.c:1530-1532`); already printed against today's emulator. The core then pushes VLAN
  tags into the packet (`C7215:net/core/dev.c:3981-3988`), so `IONIC_TXQ_DESC_FLAG_VLAN` is never set.
- RX_SG is not a negotiated feature in the driver at all; RX scatter is selected per queue by
  `IONIC_QINIT_F_SG` in RXQ Q_INIT (`K74:ionic_lif.c:939-941`).

### 1.6 Emulator gaps (VERIFIED in `E:`)

- G-a1. `handle_lif_setattr()` acts only on MAC and leaves `comp[4..11]` zero
  (`E:src/ionic/ionic_eth_emu.c:1525-1535`); the adminq loop then fills comp_index and colour
  (`E:...:1888-1904`). Needs: for `attr == 4`, `ernic_put_le64(comp + 4, ernic_get_le64(cmd + 4) & SUPPORTED)`.
- G-a2. LIF identity `features` at 0x0a8 left zero (`E:...:1142`). Informational, set it to SUPPORTED.
- G-a3. Docs say no offloads (`E:docs/ionic.rst:361-366`); CHANGELOG entry required by `E:AGENTS.md`.

---------------------------------------------------------------------------------------------------

## 2. (b) Q_IDENTIFY for the TXQ, and TXQ Q_INIT

### 2.1 Command and identity layout (VERIFIED `K74:ionic_if.h:679-731`; `K726:ionic_if.h:650-702`)

```
struct ionic_q_identify_cmd:  0 u8 opcode, 1 u8 rsvd, 2 le16 lif_type, 4 u8 type, 5 u8 ver
        ver = "Highest queue type version that the driver supports"
struct ionic_q_identify_comp: 0 u8 status, 1 rsvd, 2 le16 comp_index, 4 u8 ver
union ionic_q_identity (devcmd data window):
        0  u8   version         "Queue type version that can be used with FW"
        1  u8   supported       bitfield of versions, bit 0 = ver 0
        8  le64 features        enum ionic_q_feature
        16 le16 desc_sz
        18 le16 comp_sz
        20 le16 sg_desc_sz
        22 le16 max_sg_elems
        24 le16 sg_desc_stride  "Number of Scatter/Gather elements per descriptor"
```

The driver asks with `ver = 3` for TXQ, 2 for RXQ, 0 for adminq/notifyq
(VERIFIED `K74:ionic_lif.c:28-38,3938-3939`):

```c
	[IONIC_QTYPE_TXQ]     = 3,   /* 0 = Base version with CQ+SG support
				      * 1 =       ... with Tx SG version 1
				      * 3 =       ... with CMB rings
				      */
```

### 2.2 What the driver does with each field (VERIFIED `K74:ionic_lif.c:3942-3991`)

| Field | Use |
| --- | --- |
| `version` | stored; sent back as `q_init.ver` (`K74:ionic_lif.c:844`); `>= 1` is half of the v1-SG test |
| `supported` | `dev_dbg` only |
| `features` | `IONIC_QIDENT_F_CMB` gates ethtool CMB rings (`K74:ionic_ethtool.c:609,620`); the RDMA driver reads EXPDB (per `E:...:1624-1630`). Nothing else. Keep 0. |
| `desc_sz`, `comp_sz` | `dev_dbg` only; the driver always uses `sizeof(struct ionic_txq_desc)` = 16 and `sizeof(struct ionic_txq_comp)` = 16 (`K74:ionic_lif.c:2088-2089`) |
| `sg_desc_sz` | `== 256` is the other half of the v1-SG test |
| `max_sg_elems` | **overwritten** (below) |
| `sg_desc_stride` | `dev_dbg` only |

The override (VERIFIED `K74:ionic_lif.c:3982-3989`, `K74:ionic_dev.h:213`; `K726:ionic_lif.c:3979-3986`):

```c
		if (qtype == IONIC_QTYPE_TXQ)
			max_frags = IONIC_TX_MAX_FRAGS;          /* 1 + IONIC_TX_MAX_SG_ELEMS_V1 = 16 */
		...
		qti->max_sg_elems = min_t(u16, max_frags - 1, MAX_SKB_FRAGS);
```

`MAX_SKB_FRAGS` is 17..45 (`C7215:net/Kconfig:295-296`), so **`q->max_sg_elems == 15` for every TXQ,
whatever the device says and whatever the SG version** (copied to the queue at `K74:ionic_lif.c:616`).

SG descriptor size selection (VERIFIED `K74:ionic_lif.c:2091-2096`, same test at 1004-1008 and 2989-2994;
`K726:ionic_lif.c:2088-2093`):

```c
	if (lif->qtype_info[IONIC_QTYPE_TXQ].version >= 1 &&
	    lif->qtype_info[IONIC_QTYPE_TXQ].sg_desc_sz ==
					  sizeof(struct ionic_txq_sg_desc_v1))
		sg_desc_sz = sizeof(struct ionic_txq_sg_desc_v1);
	else
		sg_desc_sz = sizeof(struct ionic_txq_sg_desc);
```

SG layouts (VERIFIED `K74:ionic_if.h:1006-1026`, sizes asserted at `K74:ionic_dev.h:130-131`):

```c
struct ionic_txq_sg_elem { __le64 addr; __le16 len; __le16 rsvd[3]; };          /* 16 bytes */
struct ionic_txq_sg_desc {                      /* v0: 8 elems, 128 bytes per TX descriptor */
#define IONIC_TX_MAX_SG_ELEMS		8
#define IONIC_TX_SG_DESC_STRIDE		8
	struct ionic_txq_sg_elem elems[IONIC_TX_MAX_SG_ELEMS];
};
struct ionic_txq_sg_desc_v1 {                   /* v1: 16 slots (15 usable), 256 bytes */
#define IONIC_TX_MAX_SG_ELEMS_V1		15
#define IONIC_TX_SG_DESC_STRIDE_V1		16
	struct ionic_txq_sg_elem elems[IONIC_TX_SG_DESC_STRIDE_V1];
};
```

| Device answers | Driver SG slot per TX descriptor | Elements the driver may write |
| --- | --- | --- |
| `version = 0` (any size), or `version >= 1` with `sg_desc_sz != 256` | 128 bytes, slot of desc `i` at `sg_ring_base + i*128` | **up to 15** (`max_sg_elems` override) -> overruns the 8-element slot into slot `i+1` |
| `version >= 1` and `sg_desc_sz == 256` | 256 bytes, at `sg_ring_base + i*256` | up to 15 (fits; `nsge` is a 4-bit field, max 15) |

The v0 overrun is VERIFIED as a code path: `ionic_tx_skb_frags()` writes `nr_frags` elements with no
bound but `nr_frags <= q->max_sg_elems` (`K74:ionic_txrx.c:1641-1645,1720`), and
`ionic_tx_sg_elems()` picks the 128-byte array when the size is not 256 (`K74:ionic_txrx.c:105-111`).
That it corrupts a later frame is INFERRED (slot `i+1` is rewritten when descriptor `i+1` is filled,
which can be before the device has read descriptor `i`). **An emulator granting TX_SG must answer TXQ
version 1 with sg_desc_sz 256.**

Recommended TXQ identity: `version = min(cmd.ver, 1)`, `supported = 0x03`, `features = 0`,
`desc_sz = 16`, `comp_sz = 16`, and for version 1 `sg_desc_sz = 256`, `max_sg_elems = 15`,
`sg_desc_stride = 16`; completion `ver` = same version. For a driver asking `ver = 0`, keep 128/8/8.
(INFERRED that real firmware returns `min(requested, supported)`; the field doc only says "version that
can be used with FW".)

### 2.3 TXQ Q_INIT (VERIFIED `K74:ionic_if.h:767-790`, `K74:ionic_lif.c:833-856`; `K726` same lines)

```
0  u8 opcode  1 rsvd  2 le16 lif_index  4 u8 type  5 u8 ver  6 rsvd1[2]
8  le32 index   12 le16 pid   14 le16 intr_index   16 le16 flags   18 u8 cos   19 u8 ring_size (log2)
20 le64 ring_base   28 le64 cq_ring_base   36 le64 sg_ring_base   44 rsvd2[12]   56 le64 features
flags: IRQ 0x01, ENA 0x02, SG 0x04, EQ 0x08, CMB 0x10, DEBUG 0x80
```

```c
			.ver = lif->qtype_info[q->type].version,
			.flags = cpu_to_le16(IONIC_QINIT_F_IRQ |
					     IONIC_QINIT_F_SG),
			...
			.sg_ring_base = cpu_to_le64(q->sg_base_pa),
			.features = cpu_to_le64(q->features),
```

- TXQ flags are always `IRQ | SG` (0x05); never `ENA` (enabled later by Q_CONTROL); `CMB` only if the
  device offered CMB TX rings. The SG ring is always allocated and its base always sent, independent of
  the LIF TX_SG feature.
- `ver` echoes the identified version: the emulator should record it per queue and derive the SG stride
  from it (256 if `ver >= 1`, else 128).
- `features` is 0 for ordinary TXQs; `IONIC_Q_F_2X_CQ_DESC | IONIC_TXQ_F_HWSTAMP` only for the hwstamp
  TXQ, which exists only with a PHC (`K74:ionic_lif.c:998`).
- CQ ring has the same depth as the TXQ (`K74:ionic_lif.c:629`), 16-byte entries.

### 2.4 Emulator gaps (VERIFIED in `E:`)

- G-b1. Q_IDENTIFY answers version 0 / `supported = 1` / TXQ 128-byte SG, 8 elems
  (`E:src/ionic/ionic_eth_emu.c:1605-1617,1640-1641`; `E:src/ionic/ionic_eth_emu_priv.h:362-363`). Ignores `cmd[5]` (`ver`).
- G-b2. Q_INIT ignores `cmd[5]` (`ver`); stores `sg` flag but TX never checks it (`E:...:1748`, gather keyed
  on `nsge` alone at `E:...:1959`). A descriptor with `nsge != 0` on a queue without `IONIC_QINIT_F_SG`
  or with `sg_ring_base == 0` should be a desc-data error.
- G-b3. SG stride is the compile-time `TXQ_SG_MAX_ELEMS * TXQ_SG_ELEM_SIZE` (`E:...:1973-1975`).

---------------------------------------------------------------------------------------------------

## 3. (c) The TX descriptor

### 3.1 Layout and `cmd` encoding (VERIFIED `K74:ionic_if.h:817-999`; `K726:ionic_if.h:788-970`)

```
struct ionic_txq_desc, 16 bytes, all little-endian
0  le64 cmd      bits  3:0   flags
                 bits  7:4   opcode
                 bits 11:8   num_sg_elems (nsge), 0..15
                 bits 63:12  addr (52 bits): DMA address of the FIRST data buffer
8  le16 len      length of the FIRST data buffer
10 le16 vlan_tci (hword0)
12 le16 csum_start | hdr_len      (hword1; which one depends on opcode)
14 le16 csum_offset | mss         (hword2)

opcode: CSUM_NONE 0, CSUM_PARTIAL 1, CSUM_HW 2, TSO 3
flags, all opcodes:   VLAN 0x1, ENCAP 0x2
flags, CSUM_HW only:  CSUM_L3 0x4, CSUM_L4 0x8
flags, TSO only:      TSO_SOT 0x4, TSO_EOT 0x8
```

```c
	cmd = (opcode & IONIC_TXQ_DESC_OPCODE_MASK) << IONIC_TXQ_DESC_OPCODE_SHIFT;   /* 0xf << 4  */
	cmd |= (flags & IONIC_TXQ_DESC_FLAGS_MASK) << IONIC_TXQ_DESC_FLAGS_SHIFT;    /* 0xf << 0  */
	cmd |= (nsge & IONIC_TXQ_DESC_NSGE_MASK) << IONIC_TXQ_DESC_NSGE_SHIFT;       /* 0xf << 8  */
	cmd |= (addr & IONIC_TXQ_DESC_ADDR_MASK) << IONIC_TXQ_DESC_ADDR_SHIFT;       /* 52b << 12 */
```

The SG elements of descriptor `i` are in SG ring slot `i` (same index): the driver takes both from
`q->head_idx` (VERIFIED `K74:ionic_txrx.c:1522-1523,1641`).

### 3.2 CSUM_NONE (opcode 0)

Driver (VERIFIED `K74:ionic_txrx.c:1597-1631`): flags = VLAN? | ENCAP?; `nsge = nr_frags`;
`len = skb_headlen`; `vlan_tci` if VLAN else 0; `csum_start = csum_offset = 0`. Also used for XDP frames.

Device: transmit head + SG bytes unchanged (plus VLAN insertion if flagged). "requires no further
manipulation from device" (`K74:ionic_if.h:828-833`).

### 3.3 CSUM_PARTIAL (opcode 1): the only checksum opcode Linux uses

Driver (VERIFIED `K74:ionic_txrx.c:1558-1595`), chosen when `skb->ip_summed == CHECKSUM_PARTIAL`
(`K74:ionic_txrx.c:1663-1666`):

```c
	cmd = encode_txq_desc_cmd(IONIC_TXQ_DESC_OPCODE_CSUM_PARTIAL,
				  flags, skb_shinfo(skb)->nr_frags,
				  buf_info->dma_addr);
	desc->cmd = cpu_to_le64(cmd);
	desc->len = cpu_to_le16(buf_info->len);
	...
	desc->csum_start = cpu_to_le16(skb_checksum_start_offset(skb));
	desc->csum_offset = cpu_to_le16(skb->csum_offset);
```

Interface text (VERIFIED `K74:ionic_if.h:835-866`): "The L4 checksum is calculated starting at
@csum_start bytes into the packet to the end of the packet. The checksum insertion position is given in
@csum_offset, which is the offset from @csum_start to the checksum field in the L4 header ... Software
will preload the L4 checksum field with the IP pseudo-header checksum."

Device algorithm (this is exactly `skb_checksum_help()`, VERIFIED `C7215:net/core/dev.c:3637-3658`:
`csum = skb_checksum(skb, offset, skb->len - offset, 0); ... *(__sum16 *)(skb->data + offset) =
csum_fold(csum) ?: CSUM_MANGLED_0;`):

1. `total` = whole frame length (section 4), offsets counted from byte 0 of the frame **as posted**
   (before any VLAN tag the device inserts).
2. Require `csum_start + csum_offset + 2 <= total` (and `csum_start < total`); otherwise drop as
   `tx_desc_data_error`.
3. **Seed**: already in the frame. The 16-bit field at `csum_start + csum_offset` holds the folded,
   *un-complemented* pseudo-header sum (addresses, protocol, L4 length). Do not zero it; it is summed
   with everything else.
4. **Range**: one's-complement sum of bytes `[csum_start, total)`, big-endian 16-bit words, odd last byte
   padded with zero.
5. **Store**: `~fold(sum)` big-endian at `csum_start + csum_offset`. The kernel stores 0xffff when the
   result is 0 (required for UDP, equivalent for TCP). Note `E:src/net/net_headers.h:257-259` deliberately
   returns 0 unchanged; for this path follow the kernel (0 -> 0xffff) at least when the L4 protocol is UDP.
6. No header parsing is needed: no L3/L4 protocol knowledge, works for TCP/UDP over IPv4/IPv6, with
   in-packet VLAN tags, and for the inner header of an encapsulated packet.

ENCAP flag on CSUM_PARTIAL: set when `skb->encapsulation`. The interface says the device "will offload
the outer header checksums using LCO". INFERRED: for Linux the device may ignore ENCAP here, because the
stack completes the outer IPv4 header checksum and (via LCO in `udp_set_csum()`) any outer UDP checksum in
software for non-GSO packets, and tunnel GSO is not granted so those are segmented in software first.
Treat as an open item if tunnels are ever tested.

SCTP CRC32c: the driver counts `crc32_csum` but uses the same opcode (`K74:ionic_txrx.c:1591-1594`).
`NETIF_F_SCTP_CRC` is never advertised, so the core does the CRC in software
(`C7215:net/core/dev.c:3990-3993`) and such a descriptor is not expected (INFERRED).

### 3.4 CSUM_HW (opcode 2): never emitted by Linux

VERIFIED: `grep -rn "OPCODE_CSUM_HW\|FLAG_CSUM_L[34]" drivers/net/ethernet/pensando drivers/infiniband/hw/ionic`
at v7.4-rc3 hits only the definitions in `ionic_if.h:820,868,954-955`. Semantics per the header
(`K74:ionic_if.h:868-873`): "If @csum_l3 is set then the packet's L3 checksum is updated. Similarly, if
@csum_l4 is set the L4 checksum is updated. If @encap is set then encap header checksums are also
updated." i.e. the device parses the headers itself: CSUM_L3 (0x4) = IPv4 header checksum, CSUM_L4 (0x8)
= full TCP/UDP checksum including pseudo-header (INFERRED: nothing is preloaded), ENCAP = same for the
outer headers. `hdr_len` is documented as valid for "CALC_CSUM" = headers up to the inner-most L4 header
(`K74:ionic_if.h:920-931`). Recommendation: the emulator rejects opcode 2 (drop, `tx_desc_data_error`)
rather than guessing; DPDK/other drivers are out of scope.

### 3.5 TSO (opcode 3)

#### What the driver posts (VERIFIED `K74:ionic_txrx.c:1399-1556`; identical lines at `K726`)

Preparation of the header template, done once on the skb head:

```c
	if (skb->protocol == cpu_to_be16(ETH_P_IP)) {
		ip_hdr(skb)->check = 0;
		tcp_hdr(skb)->check =
			~csum_tcpudp_magic(ip_hdr(skb)->saddr,
					   ip_hdr(skb)->daddr,
					   0, IPPROTO_TCP, 0);
	} else if (skb->protocol == cpu_to_be16(ETH_P_IPV6)) {
		tcp_v6_gso_csum_prep(skb);
	}
```

and (VERIFIED `C7215:include/net/ip6_checksum.h:65-72`):

```c
	ipv6h->payload_len = 0;
	th->check = ~tcp_v6_check(0, &ipv6h->saddr, &ipv6h->daddr, 0);
```

So in the template: IPv4 `check == 0`; IPv6 `payload_len == 0`; TCP `check` = folded un-complemented
pseudo-header sum over {saddr, daddr, protocol 6, **length 0**}. IPv4 `tot_len` and `id` are whatever the
stack left (the super-packet's values); the driver does not touch them.

Chunking loop:

```c
	len = skb->len;
	mss = skb_shinfo(skb)->gso_size;
	...
	hdrlen = skb_tcp_all_headers(skb);            /* inner headers if encap */
	tso_rem = len;
	seg_rem = min(tso_rem, hdrlen + mss);
	start = true;
	while (tso_rem > 0) {
		...
		while (seg_rem > 0) {
			if (frag_rem == 0) {              /* next buffer: bufs[0] = linear head, then frags */
				frag_addr = buf_info->dma_addr;
				frag_rem = buf_info->len;
				buf_info++;
			}
			chunk_len = min(frag_rem, seg_rem);
			if (!desc) {                      /* first chunk -> main descriptor addr/len */
				desc = &q->txq[q->head_idx];
				elem = ionic_tx_sg_elems(q);
				desc_addr = frag_addr;
				desc_len = chunk_len;
			} else {                          /* later chunks -> SG elements */
				elem->addr = cpu_to_le64(frag_addr);
				elem->len = cpu_to_le16(chunk_len);
				elem++;
				desc_nsge++;
			}
			...
		}
		seg_rem = min(tso_rem, mss);
		done = (tso_rem == 0);
		ionic_tx_tso_post(netdev, q, desc, skb, desc_addr, desc_nsge,
				  desc_len, hdrlen, mss, outer_csum, vlan_tci,
				  has_vlan, start, done);
		start = false;
		...
	}
```

and `ionic_tx_tso_post()` writes, into **every** descriptor of the chain: opcode TSO, flags
`VLAN? | ENCAP? | SOT? | EOT?`, `nsge`, `addr`, `len` (first buffer of that descriptor), `vlan_tci`,
`hdr_len = hdrlen`, `mss` (`K74:ionic_txrx.c:1411-1421`).

Resulting contract (VERIFIED from the loop; "Each desc is mss long max, so a descriptor for each gso_seg",
`K74:ionic_txrx.c:1710`, `K726:...:1703`):

- **One descriptor = exactly one wire segment.** A TSO skb is a chain of
  `N = DIV_ROUND_UP(skb->len - hdr_len, mss)` consecutive descriptors.
- Descriptor 0 has `TSO_SOT` and carries `hdr_len` bytes of headers followed by the first
  `min(mss, payload)` payload bytes. Total bytes of descriptor 0 = `len + sum(sg.len)` = `hdr_len + mss`
  (or less if it is also the last).
- Descriptors 1..N-1 carry **payload only**, exactly `mss` bytes each except the last, which carries the
  remainder (1..mss) and has `TSO_EOT`. They repeat `hdr_len`/`mss`/`vlan_tci`/VLAN/ENCAP identically.
- A one-segment chain has `SOT|EOT` on one descriptor.
- Per descriptor, at most `1 + max_sg_elems = 16` buffers; `ionic_tx_descs_needed()` linearizes the skb
  otherwise (`K74:ionic_txrx.c:1745-1778`).
- INFERRED: the header bytes are always at the start of descriptor 0's *first buffer* (the linear head is
  `bufs[0]` and the stack keeps L2..L4 headers linear; the driver itself dereferences `tcp_hdr()`), but
  nothing forces `desc0.len >= hdr_len` in the loop, so the device should take the first `hdr_len` bytes
  of descriptor 0's gathered bytes (head + SG), not of its first buffer.
- The skb and all DMA mappings are recorded on descriptor 0's `tx_info` only
  ("Buffer information is stored with the first tso descriptor", `K74:ionic_txrx.c:1494-1495,1545-1547`).
- Doorbell: none after SOT, one after EOT (`K74:ionic_txrx.c:1425-1432`). For `SOT|EOT` in one
  descriptor **no doorbell is rung** by this skb (the `start` branch wins); it goes out with the next
  doorbell. `ionic_txq_poke_doorbell()` only runs when `lif->doorbell_wa` (`K74:ionic_txrx.c:971,1036`).
  INFERRED rare: TCP only marks an skb GSO when `skb->len > mss` (`C7215:net/ipv4/tcp_output.c:1757-1769`).
  The device always sees a complete chain below the doorbell's p_index, except in that late-doorbell case
  where it simply sees it later.

#### What the device must do per chain

Interface text (VERIFIED `K74:ionic_if.h:875-900`): "@hdr_len is the number of bytes to the end of TCP
header (the offset to the TCP payload). @mss is the desired MSS, the TCP payload length for each segment.
The device will calculate/insert IP (IPv4 only) and TCP checksums for each segment. In the first data
buffer containing the header template, the driver will set IPv4 checksum to 0 and preload TCP checksum
with the IP pseudo header calculated with IP length = 0. ... TCP ECN offload is supported. The device will
set CWR flag in the first segment if CWR is set in the template header, and clear CWR in remaining
segments."

For segment `i` (0-based) with payload `P_i` bytes at payload offset `off_i = i * mss`:

1. Build `template(hdr_len bytes) + payload(P_i)`. The device must locate L3 and L4 itself: the
   descriptor gives only `hdr_len` (end of TCP header). Parse Ethernet -> optional 802.1Q/802.1ad tags ->
   IPv4 (IHL) or IPv6 (40 bytes + extension headers) -> TCP (`doff`), and check
   `l4_off + doff*4 == hdr_len`; reject the chain otherwise.
2. **IPv4**: `tot_len = (hdr_len - l3_off) + P_i`; `id = template_id + i` (the driver advertises no
   `NETIF_F_TSO_MANGLEID`, so the core never hands it a fixed-ID TSO skb; software reference
   `C7215:net/ipv4/af_inet.c:1434-1450`); then recompute the IPv4 header checksum over IHL*4 bytes
   (template has 0).
3. **IPv6**: `payload_len = (hdr_len - l3_off - 40) + P_i` (template has 0). No header checksum.
4. **TCP seq**: `seq = template_seq + off_i`.
5. **TCP flags**: FIN and PSH only on the last segment (clear on all others); CWR only on the first
   (clear on the rest). Reference software behaviour, VERIFIED `C7215:net/ipv4/tcp_offload.c:208-229`:
   `th->fin = th->psh = 0;` for every segment but the last, `th->seq = htonl(seq); th->cwr &= ecn_cwr_mask;`
   for every segment after the first.
6. **TCP checksum**: the template field holds the pseudo-header sum with length 0. Compute
   `sum = csum(bytes [l4_off, hdr_len + P_i))` with the (rewritten) header as it stands, the seed still in
   the check field, **plus the TCP length `(hdr_len - l4_off) + P_i` as one 16-bit big-endian word**
   (that is the missing pseudo-header length; same arithmetic for IPv4 and IPv6); store `~fold(sum)` in
   the check field. Each segment must start from the template's seed, not from the previous segment's
   result.
7. VLAN flag: insert `0x8100, vlan_tci` after the 12 address bytes of every segment, after the offsets
   above have been applied (offsets are relative to the untagged frame).
8. ENCAP flag (`outer_csum`, set only for GRE/IPXIP/UDP-tunnel gso types, `K74:ionic_txrx.c:1483-1488`):
   outer IP length/ID/checksum and outer UDP length/checksum per segment. Not needed if the tunnel TSO
   bits are not granted: the core then segments those skbs in software (INFERRED from the core's
   `gso_type`-vs-features test; not re-read here).
9. Emit N frames, each `hdr_len + P_i` (+4 with VLAN) bytes, in order.

Checks the device should make (reject the whole chain, count `tx_desc_data_error`, still complete it):
first descriptor of a chain must have SOT and nothing else may; `mss != 0`; `hdr_len` >= 54 and
`<=` descriptor 0's bytes; every descriptor's `hdr_len`/`mss` equal to the SOT's; non-SOT descriptor
bytes in 1..mss; `hdr_len + mss (+4)` fits the frame buffer; a chain that hits the producer index without
EOT must wait (not be transmitted partially).

Caveats:

- `NETIF_F_TSO6` with **IPv6 extension headers**: VERIFIED `C7215:net/core/dev.c:3895-3900` strips TSO6
  for extension headers only when the device has `NETIF_F_IPV6_CSUM`; ionic advertises `NETIF_F_HW_CSUM`
  instead, so such skbs can reach the device. Walk hop-by-hop/routing/destination-options headers or
  reject (and say so in the changelog), or leave `TSO_IPV6` ungranted initially.
- `TSO_ECN`: without the bit the core software-segments skbs carrying `SKB_GSO_TCP_ECN` (INFERRED), so
  CWR handling is only reachable when it is granted; implementing the CWR rule is three lines anyway.
- Chain length: no `gso_max_segs` is set by the driver, so N is bounded only by 64 KiB / mss and the ring
  (default 1024 descriptors, `K74:ionic_dev.h:20`; "44 /* 64K TSO @1500B */", `K74:ionic_dev.h:23`).

### 3.6 Emulator gaps for (c) (VERIFIED `E:src/ionic/ionic_eth_emu.c:1932-2015`)

- G-c1. Opcode and flags never decoded (`E:...:1944-1949` reads only nsge and addr).
- G-c2. `csum_start`/`csum_offset`/`hdr_len`/`mss`/`vlan_tci` never read; no checksum, no TSO, no VLAN.
- G-c3. No header parser/segmenter. Checksum primitives exist: `net_csum_add()`/`net_csum_fold()`
  (`E:src/net/net_headers.h:268-290`).
- G-c4. One descriptor in, one frame out, one completion: no chain state on `struct eth_queue`
  (`E:src/ionic/ionic_eth_emu_priv.h:414-429`).
- G-c5. The comment at `E:...:1951-1953` ("We advertise no offloads, so the driver never posts a TSO
  descriptor") becomes false.

---------------------------------------------------------------------------------------------------

## 4. (d) What `desc->len` means with SG elements

VERIFIED, three ways:

- Interface: "addr: First data buffer's DMA address (Subsequent data buffers are on txq_sg_desc)";
  "@len: First data buffer's length, in bytes" (`K74:ionic_if.h:915-917`; `K726:ionic_if.h:886-888`).
- Non-TSO: `desc->len = cpu_to_le16(buf_info->len)` with `buf_info = desc_info->bufs` = the linear head,
  `skb_headlen(skb)` bytes (`K74:ionic_txrx.c:1130-1134,1562,1579,1601,1618`); frags go to the SG slot
  with their own lengths (`K74:ionic_txrx.c:1641-1645`).
- TSO: `desc_len = chunk_len` of the first chunk only (`K74:ionic_txrx.c:1520-1526`).
- XDP: `desc->len = cpu_to_le16(len)` of the first buffer, frags in SG (`K74:ionic_txrx.c:384`).

**Total bytes of a descriptor = `desc.len + sum_{k < nsge} sg[k].len`.** There is no total-length field.
For CSUM_NONE/CSUM_PARTIAL that total is the frame; for TSO it is one segment's payload (plus headers on
SOT).

Emulator gap G-d1 (VERIFIED `E:...:1951-1957,1981-1997`): treats `len` as the whole frame,
`head_len = len - sg_total`, drops when `sg_total >= len`. Any real SG frame whose frags total >= the head
(every devmem/zerocopy TCP data segment: ~66-byte head, payload in frags) is dropped as
`tx_desc_data_error`; others are truncated. The Python client encodes the same wrong convention
(`E:tests/py/ernic/eth.py:215-236`, used with `split=` by `E:tests/py/test_p4.py:87`) and must change with it.

Required bounds once fixed: `len != 0`; each SG element `len != 0` (the driver never posts an empty frag;
INFERRED, reject rather than skip as `E:...:2004-2006` does today); running total checked **before** each
`ernic_dma_read()` against `sizeof(emu->frame)` (9600, `E:src/ionic/ionic_eth_emu_priv.h:535`) minus 4
when VLAN is to be inserted; `nsge <= 15` for a v1 queue, `<= 8` for v0.

---------------------------------------------------------------------------------------------------

## 5. (e) Completion semantics

Layout (VERIFIED `K74:ionic_if.h:1036-1042`, 16 bytes):

```
0 u8 status   1 u8 rsvd   2 le16 comp_index   4 u8 rsvd2[11]   15 u8 color (bit 7 = IONIC_COMP_COLOR_MASK 0x80)
```

Driver (VERIFIED `K74:ionic_txrx.c:1238-1315`; identical at `K726`):

```c
	if (!color_match(comp->color, cq->done_color))
		return false;
	dma_rmb();
	/* clean the related q entries, there could be
	 * several q entries completed for each cq completion
	 */
	do {
		desc_info = &q->tx_info[q->tail_idx];
		desc_info->bytes = 0;
		index = q->tail_idx;
		q->tail_idx = (q->tail_idx + 1) & (q->num_descs - 1);
		ionic_tx_clean(q, desc_info, comp, in_napi);
		if (desc_info->skb) {
			pkts++;
			bytes += desc_info->bytes;
			desc_info->skb = NULL;
		}
	} while (index != le16_to_cpu(comp->comp_index));
```

```c
	while (ionic_tx_service(cq, &pkts, &bytes, in_napi)) {
		if (cq->tail_idx == cq->num_descs - 1)
			cq->done_color = !cq->done_color;
		cq->tail_idx = (cq->tail_idx + 1) & (cq->num_descs - 1);
```

- `color`: a CQ entry is valid when bit 7 equals `done_color`; `done_color` starts at 1 and flips each
  time the CQ index wraps. The emulator already does this (`E:...:1750-1751,1922-1928,2081-2082`).
- `comp_index`: TXQ index of the **last descriptor this completion covers**. The driver cleans every
  descriptor from its tail up to and including `comp_index`. So completions are cumulative: the device
  may write one per descriptor or one per several, in order. A `comp_index` outside `[tail, head)` makes
  the loop run past the head (VERIFIED from the loop: nothing bounds it), so it must be exact.
- `status`: **never read** by the TX path (VERIFIED: `ionic_tx_clean()`/`ionic_tx_service()` use only
  `color`, `comp_index`, and the hwstamp bytes in front of a 2x CQ descriptor). A drop cannot be signalled
  to the driver through the completion; only through LIF stats.
- The interrupt and credit return are unchanged from today.

Multi-descriptor TSO (VERIFIED `K74:ionic_txrx.c:1185-1236,1494-1495,1545-1547`): the skb and its buffer
list hang off the **first** descriptor of the chain; the other descriptors have `skb == NULL`,
`nbufs == 0`. Cleaning descriptor 0 therefore unmaps every buffer of the skb and frees it
(`ionic_tx_desc_unmap_bufs()` + `napi_consume_skb()`); cleaning the rest is a no-op. BQL/`pkts` count the
skb once, at descriptor 0.

Consequence for the device (the reasoning is INFERRED, the driver facts are VERIFIED): a completion that
covers the SOT descriptor releases the pages of the *whole* TSO skb. If the emulator wrote the SOT's
completion before it had read the later descriptors' buffers, a guest NAPI on another vCPU could free and
reuse those pages mid-gather (and, for MSG_ZEROCOPY/devmem, signal the application that the buffer is
reusable). **Rule: read all data of a chain first, then complete it; simplest is a single completion per
chain with `comp_index` = index of the EOT descriptor.** The emulator today completes each descriptor
right after gathering it (`E:...:2048-2094`), which is correct only for single-descriptor packets.

Emulator gap G-e1: status is 0 and a completion is written even when the frame was dropped
(`E:...:2052-2054,2078-2090`). That matches what the driver can consume; keep it, but make the drop
counters trustworthy (section 6) because they are the only signal.

---------------------------------------------------------------------------------------------------

## 6. (f) Stats the driver reads

Device-provided (VERIFIED `K74:ionic_lif.c:1285-1336`, struct at `K74:ionic_if.h:2969-3050`,
1024 bytes per `K74:ionic_dev.h:59`): `ndo_get_stats64` reads `lif->info->stats` (the LIF info DMA region):

```c
	ns->tx_packets = tx_ucast_packets + tx_mcast_packets + tx_bcast_packets;
	ns->tx_bytes   = tx_ucast_bytes + tx_mcast_bytes + tx_bcast_bytes;
	ns->tx_dropped = tx_ucast_drop_packets + tx_mcast_drop_packets + tx_bcast_drop_packets;
	ns->tx_aborted_errors = tx_dma_error + tx_queue_disabled + tx_desc_fetch_error + tx_desc_data_error;
	ns->tx_errors = ns->tx_aborted_errors;
```

`ethtool -S` shows the same through `hw_tx_dropped` / `hw_tx_aborted_errors`
(`K74:ionic_stats.c:25,29,229,233`).

Driver-side software counters, useful as test oracles because they prove which path the driver took
(VERIFIED `K74:ionic_lif.h:26-42`, `K74:ionic_stats.c:14-21,131-144,173-186,288`):

- LIF totals: `tx_packets`, `tx_bytes`, `tx_tso`, `tx_tso_bytes`, `tx_csum_none`, `tx_csum`.
- Per queue `tx_<q>_...`: `pkts`, `bytes`, `clean`, `dma_map_err`, `linearize`, `frags`, `tso`,
  `tso_bytes`, `csum_none`, `csum`, `vlan_inserted`.
- `tx_csum` increments per CSUM_PARTIAL descriptor (`K74:ionic_txrx.c:1594`); `frags` by `nr_frags`
  (`:1647`); `tso` per TSO skb and `pkts += DIV_ROUND_UP(len - hdrlen, mss)` (`:1550-1553`);
  `linearize` when an skb had too many frags (`:1777`).

What the emulator should count (VERIFIED current behaviour `E:src/ionic/ionic_emu_stats.c:64-121`,
`E:src/ionic/ionic_eth_emu.c:2052-2072`): today one frame per descriptor. With TSO, count **each emitted
segment** in `tx_*_packets/bytes` (wire frames, so the guest's `ip -s link` matches the peer), and count a
rejected chain once in `tx_desc_data_error`. A checksum-range or TSO-header reject should use
`IONIC_EMU_DROP_DESC_DATA`.

---------------------------------------------------------------------------------------------------

## 7. Gap list (all VERIFIED against `E:` at bd902345)

| # | Area | Today | Needed |
| --- | --- | --- | --- |
| G-a1 | LIF_SETATTR FEATURES | comp features = 0 (`ionic_eth_emu.c:1525-1535`) | `comp[4..11] = cmd[4..11] & SUPPORTED` |
| G-a2 | LIF identity features | 0 (`:1142`) | SUPPORTED (no TIMESTAMP bits) |
| G-b1 | Q_IDENTIFY TXQ | v0, 128 B, 8 elems, ignores `cmd.ver` (`:1605-1617,1640-1641`) | v1, 256 B, 15 elems, stride 16; v0 answer if `cmd.ver == 0` |
| G-b2 | Q_INIT | `ver` ignored; `sg` flag unused on TX (`:1748,1959`) | store ver; SG stride per queue; nsge needs SG flag + ring |
| G-b3 | SG stride | constant 128 (`:1973-1975`) | 256 on v1 queues |
| G-d1 | `len` | whole frame, head = len - sg_total (`:1951-1997`) | first buffer; total = len + sum(sg) with bound per read |
| G-c1/2 | opcode, flags, hwords | ignored (`:1944-1949`) | decode; reject unknown opcode/flag combinations |
| G-c3 | CSUM_PARTIAL | pass-through | fill per section 3.3 |
| G-c4 | TSO | each descriptor sent raw | chain state, header template, per-segment rewrite (3.5) |
| G-e1 | completion per descriptor | yes (`:2078-2093`) | per chain for TSO (after all data is read) |
| G-f1 | stats | per descriptor | per emitted segment; rejects in `tx_desc_data_error` |
| G-t1 | tests/py client | emulator's `len` convention (`tests/py/ernic/eth.py:215-236`) | driver convention; add opcode/hword fields |
| G-x1 | docs/changelog | "no offloads" (`docs/ionic.rst:361-366`) | update; CHANGELOG `Added`/`Changed` |

Unchanged and fine: colour handling, one IRQ per service pass, TX filter/TAP sink, BME gate.

---------------------------------------------------------------------------------------------------

## 8. Minimal ordered implementation plan (emulator; not done here)

### Stage 1: SG + CSUM_PARTIAL + correct `len` + v1 SG descriptors

1. Constants in `ionic_eth_emu_priv.h` as `_OFF`/mask macros: opcode shift 4, flags mask, nsge shift 8,
   addr shift 12; `TXQ_DESC_LEN_OFF 8`, `VLAN_TCI_OFF 10`, `HWORD1_OFF 12`, `HWORD2_OFF 14`; opcodes 0..3;
   flags; `TXQ_SG_V1_STRIDE 256`, `TXQ_SG_V1_MAX_ELEMS 15`; feature bits; `IONIC_LIF_ATTR_FEATURES 4`.
2. `struct eth_queue`: add `uint8_t ver` (from Q_INIT `cmd[5]`); refuse a TXQ `ver` above 1 with a status.
3. Q_IDENTIFY: TXQ answer `version = min(cmd[5], 1)`, `supported = 3`, v1 sizes; `comp[4]` = version.
   RXQ/adminq/notifyq unchanged.
4. `eth_tx_gather()`: `len` = first buffer; SG slot at `sg_ring_base + index * (ver ? 256 : 128)`;
   `nsge` limit by version; require `q->sg` and a non-zero SG ring for `nsge != 0`; accumulate with the
   overflow-safe form (`elen > limit - off`) before each DMA read; zero-length head or element -> DESC_DATA.
5. Opcode dispatch after gather: 0 -> as is; 1 -> checksum fill (range checks, `net_csum_add` over
   `[csum_start, total)`, store `~fold`, 0 -> 0xffff); 2 and 3 -> DESC_DATA drop in this stage; ENCAP
   ignored for opcode 1 (commented why); VLAN flag -> DESC_DATA drop (not granted).
6. LIF_SETATTR FEATURES: answer `req & (TX_SG | TX_CSUM)`; LIF identity features = same mask.
7. Tests: change `tests/py/ernic/eth.py::post_tx` (and its users) to the driver convention; unit tests in
   `tests/test_ionic_queues.c` for each new error path; docs and CHANGELOG. Order matters: steps 3+4 must
   land with or before step 6, or the guest posts 15-element SG lists into 8-element slots.

### Stage 2: TSO

1. `struct eth_queue` (or the emu) gains chain state: in-chain flag, saved header template (bounded, e.g.
   <= 256 bytes), `hdr_len`, `mss`, `l3_off`, `l4_off`, IP version, base seq, base IPv4 id, segment index,
   first descriptor index, poisoned flag.
2. Header parser (Ethernet, 802.1Q/ad, IPv4 IHL, IPv6 + extension headers or reject, TCP doff) with every
   offset bounded by `hdr_len`.
3. Service loop: on SOT validate and save; for each descriptor gather payload behind a copy of the
   template, rewrite per 3.5, send, count; on EOT write **one** completion with `comp_index` = EOT index.
   If the producer index is reached mid-chain, stop without completing and resume on the next doorbell.
   A malformed chain is consumed to its EOT, counted once, and completed.
4. Grant `TSO | TSO_IPV6` (and `TSO_ECN` with the CWR rule). Keep tunnel and VLAN bits clear.
5. Queue reset/Q_INIT/LIF reset must clear chain state.

INFERRED alternative for stage 2, not recommended: switch the TAP to `IFF_VNET_HDR` and hand the host a
GSO super-frame. It would bypass the in-process TX filters (S3, P4), change the RX read format, and still
needs the chain gather; in-emulator segmentation keeps one frame format everywhere.

---------------------------------------------------------------------------------------------------

## 9. Guest-visible tests proving each stage

Stage 1 (run in a guest against the changed emulator; peer = host TAP stack or a second guest):

1. `ethtool -k <if>`: `scatter-gather: on`, `tx-checksumming: on` (`tx-checksum-ip-generic: on`),
   `tcp-segmentation-offload: off`. `dmesg`/dyndbg shows `feature ETH_HW_TX_SG`, `ETH_HW_TX_CSUM`
   (`K74:ionic_lif.c:1542-1545`).
2. dyndbg `qtype[3].version = 1`, `sg_desc_sz = 256` (`K74:ionic_lif.c:3965-3976`); debugfs SG ring blob
   size = `num_descs * 256` (`K74:ionic_debugfs.c:157-162`).
3. TCP bulk transfer guest -> host with payload hash check both ends; `ethtool -S`: `tx_csum` rises,
   `tx_csum_none` stays flat for TCP, `tx_<q>_frags` rises, `hw_tx_aborted_errors` stays 0. On the host:
   `tcpdump -vv` on the TAP shows `cksum ... (correct)` (the TAP has no vnet header, so the host verifies);
   `nstat TcpInCsumErrors` stays 0.
4. Same over IPv6, and UDP (`iperf3 -u`, plus a datagram whose checksum computes to 0 if constructible)
   to prove the generic `csum_start`/`csum_offset` path rather than a TCP/IPv4 special case.
5. Guest -> second guest: the receiver verifies in software (no RX_CSUM), `TcpInCsumErrors` 0.
6. High-frag skbs: `sendfile()`/`MSG_ZEROCOPY`/`splice` senders with small page-crossing writes so
   `nr_frags` reaches 9..15; `tx_<q>_linearize` stays 0 and data verifies (proves v1 SG; with v0 these
   frames corrupt or drop).
7. Negative: `ethtool -K <if> tx off` -> `tx_csum` stops rising, traffic still verifies (CSUM_NONE path);
   `sg off` -> `frags` stops.
8. Emulator-side: `tx_desc_data_error`, `tx_dma_error` in `--stats-file` unchanged across the run.
9. Then the devmem TX test proper (udmabuf-backed `MSG_ZEROCOPY` + `SCM_DEVMEM_DMABUF`), receiver hash.

Stage 2:

1. `ethtool -k`: `tcp-segmentation-offload: on`, `tx-tcp-segmentation`, `tx-tcp6-segmentation` on.
2. Bulk TCP v4 and v6, payload hash; `ethtool -S` `tx_tso`, `tx_tso_bytes` rise; host `tcpdump` on the TAP
   shows wire frames <= MTU with correct IP and TCP checksums, increasing IPv4 IDs, contiguous sequence
   numbers, PSH/FIN only on the last segment of a burst; receiver `TcpInCsumErrors`/`IpInHdrErrors` 0,
   no retransmit storm (`TcpRetransSegs` near 0).
3. MSS variety: `ip route ... advmss`/`TCP_MAXSEG` small (e.g. 88, 536) and jumbo MTU; odd payload sizes
   so the last segment is 1 byte; sends of exactly k*mss.
4. ECN (`sysctl net.ipv4.tcp_ecn=1` both ends) if `TSO_ECN` is granted: CWR only on the first segment.
5. Completion/lifetime: MSG_ZEROCOPY sender that overwrites its buffer as soon as the errqueue
   notification arrives, receiver hash still correct (catches completing a chain before reading it);
   run with >= 2 vCPUs.
6. Guest packet count: `ip -s link` TX packets on the guest == frames seen on the TAP.
7. `ethtool -K <if> tso off` -> `tx_tso` flat, still correct (regression for stage 1).

---------------------------------------------------------------------------------------------------

## 10. Open questions

1. Real-firmware behaviour is not observable here: whether it writes one completion per TSO chain or per
   descriptor, what it does with ENCAP on CSUM_PARTIAL, and how it answers Q_IDENTIFY for a driver asking
   `ver = 0`. The driver tolerates either completion style; the single-completion-at-EOT rule above is the
   safe one for an emulator that reads data synchronously from another process.
2. IPv6 extension headers under TSO6 (section 3.5): walk them or reject; decide before granting bit 9.
3. Zero checksum substitution: kernel stores 0xffff for a computed 0 on every protocol; the emulator's
   helper comment (`E:src/net/net_headers.h:257-259`) argues against that for TCP. Both verify at a
   receiver; UDP requires 0xffff.
4. Driver-series interaction, not a device matter: `ionic_tx_descs_needed()` calls `skb_linearize()` when
   a non-GSO skb has more than 15 frags or a TSO segment spans more than 16 buffers
   (`K74:ionic_txrx.c:1720-1723,1754-1757,1773-1778`); that cannot work on an unreadable (devmem) skb
   (`MAX_SKB_FRAGS` is 17+).
5. Which emulator build the fleet runs (carried over from `ernic-emu-datapath.md`, open question 1).
6. Whether to gate the new feature mask behind a profile/option so existing deployments keep today's
   behaviour (a guest-visible change either way; needs a CHANGELOG entry).

---------------------------------------------------------------------------------------------------

## Verification pass

Independent re-read, 2026-11-18, of the cited source at the stated tags (same `K74:`/`K726:`/`C7215:`/`E:`
citation form as above). Read-only; nothing run against a guest. Verdicts: CONFIRMED (re-read and
agrees), WRONG (with correction), UNVERIFIABLE (cannot be settled from these trees).

### Claims re-checked

| # | Claim | Verdict | Evidence / correction |
| --- | --- | --- | --- |
| V1 | v7.2.6 and v7.4-rc3 have a byte-identical TX wire contract | CONFIRMED | `git diff v7.2.6 v7.4-rc3 -- drivers/net/ethernet/pensando/ionic/`: `ionic_if.h` adds only bit 21, RDMA identity fields, `udma_idx`, a CQ magic; `ionic_txrx.c` changes only the tail of `ionic_tx()` (`K74:ionic_txrx.c:1675-1690` vs `K726:ionic_txrx.c:1675-1683`); `ionic_dev.c`/`ionic_dev.h` unchanged; `ionic_main.c`, `ionic_bus_pci.c`, `ionic_lif.h` changes are unrelated |
| V2 | `hw_features = cmd.features & comp.features`; only the SETATTR completion matters | CONFIRMED | `K74:ionic_lif.c:1524-1525`, `K726:ionic_lif.c:1523-1524` |
| V3 | LIF identity `eth.config.features` used only for TIMESTAMP bits | CONFIRMED | only users: `K74:ionic_phc.c:582-584`, `K74:ionic_lif.c:1516-1517`, `K74:ionic_lif.c:4020` (dbg) |
| V4 | TX_SG -> `NETIF_F_SG`; TX_CSUM -> `NETIF_F_HW_CSUM` via `hw_enc_features` | CONFIRMED | `K74:ionic_lif.c:1615-1619,1641-1643` |
| V5 | Ungranted RX_CSUM/RX_HASH are safe | CONFIRMED | `K74:ionic_txrx.c:666-691`. Extra reason not to grant RX_CSUM: the driver then trusts `comp->csum` as CHECKSUM_COMPLETE (`:685-687`) |
| V6 | Driver asks TXQ `ver = 3`; uses only `version` and `sg_desc_sz`; `max_sg_elems` forced to 15 | CONFIRMED | `K74:ionic_lif.c:33,3938-3939,3982-3989` (`K726:...:3986`); `IONIC_TX_MAX_FRAGS` = 16 at `K74:ionic_dev.h:213`; other `qtype_info` readers are only `ionic_ethtool.c:609,620` (CMB) and `drivers/infiniband/hw/ionic/ionic_lif_cfg.c:94,96` (EXPDB) |
| V7 | v1 SG slots only if `version >= 1 && sg_desc_sz == 256`; otherwise 128-byte slots with up to 15 elements written | CONFIRMED | `K74:ionic_lif.c:1004-1005,2091-2096,2989-2990`; `K74:ionic_txrx.c:105-111,1641-1645,1720`. Corruption of the neighbour slot remains INFERRED |
| V8 | TXQ Q_INIT flags always `IRQ|SG`, never ENA; `ver` echoes identify | CONFIRMED | `K74:ionic_lif.c:842-856` (+CMB only with CMB rings, `:859-862`) |
| V9 | `desc->len` = first buffer only; total = `len + sum(sg.len)` | CONFIRMED | `K74:ionic_if.h:915-917`; `K74:ionic_txrx.c:1130-1135,1579,1618,1641-1645,1520-1526` |
| V10 | CSUM_HW / CSUM_L3 / CSUM_L4 never emitted | CONFIRMED | grep of `drivers/` at v7.4-rc3 hits only `ionic_if.h:820,868,954-955` |
| V11 | CSUM_PARTIAL = `skb_checksum_help()` algorithm, 0 -> 0xffff | CONFIRMED | `K74:ionic_txrx.c:1575-1588`; `C7215:net/core/dev.c:3658` |
| V12 | TSO: one descriptor per wire segment; SOT carries headers + first payload; every descriptor repeats `hdr_len`/`mss` | CONFIRMED | `K74:ionic_txrx.c:1411-1421,1494-1547,1710` |
| V13 | TSO template always arrives with IPv4 check = 0, IPv6 payload_len = 0, TCP check = pseudo-header sum with length 0 | **WRONG (incomplete)** | See correction C1 below |
| V14 | FIN/PSH on last only, CWR on first only, seq += mss | CONFIRMED (as the software reference) | `C7215:net/ipv4/tcp_offload.c:206-229`. Note AccECN skbs (`SKB_GSO_TCP_ACCECN`, `C7215:net/ipv4/tcp_output.c:357`) must keep CWR; they need a feature ionic never advertises, so the core segments them in software |
| V15 | "Core never hands a fixed-ID TSO skb" | CONFIRMED by default only | `register_netdevice` adds `NETIF_F_TSO_MANGLEID` to `hw_features` when TSO is present (`C7215:net/core/dev.c:11512-11519`), so `ethtool -K tx-tcp-mangleid-segmentation on` enables it; incrementing IDs is then still permitted behaviour |
| V16 | Completion: `status` never read, `comp_index` cumulative and unbounded, skb/mappings on SOT descriptor | CONFIRMED | `K74:ionic_txrx.c:1185-1236,1238-1281,1494-1495,1545-1547`; the only `comp->status` read in the file is RX (`:628`) |
| V17 | No doorbell after a lone `SOT|EOT` descriptor | CONFIRMED | `K74:ionic_txrx.c:1425-1432` |
| V18 | A partial TSO chain is never below the doorbell's p_index | CONFIRMED | all doorbell sites run under the TX lock (`K74:ionic_txrx.c:44-71`, `1675-1690`, `1425-1432`); the "resume a chain on the next doorbell" state in the plan is defence against a hostile guest, not a Linux code path |
| V19 | Completing SOT early lets the guest free pages mid-gather | UNVERIFIABLE here (INFERRED stands) | Driver half is CONFIRMED (V16). Emulator half: `eth_txq_service()` writes each completion inside the loop and raises the IRQ after it (`E:src/ionic/ionic_eth_emu.c:2048-2096`); whether another vCPU's NAPI can observe it mid-loop depends on the QEMU/vfio-user doorbell path, not read here |
| V20 | Stats: `ndo_get_stats64` reads the LIF stats block; ethtool names | CONFIRMED | `K74:ionic_lif.c:1285-1336`; `K74:ionic_stats.c:14-29,131-144,229,233` |
| V21 | TSO6 with IPv6 extension headers reaches the device | CONFIRMED | `C7215:net/core/dev.c:3895-3900` keys on `NETIF_F_IPV6_CSUM`, which ionic does not set |
| V22 | `skb_linearize()` in `ionic_tx_descs_needed()` "cannot work" on devmem skbs | CONFIRMED, but overstated | See correction C2 |
| V23 | Emulator gaps G-a1, G-a2, G-b1..3, G-c1..5, G-d1, G-e1, G-t1, G-x1 | CONFIRMED | `E:src/ionic/ionic_eth_emu.c:1142,1525-1535,1605-1641,1748,1932-2015,2048-2096`; `E:src/ionic/ionic_eth_emu_priv.h:362-363,414-429`; `E:tests/py/ernic/eth.py:215-236`; `E:docs/ionic.rst:361-366` |
| V24 | Real firmware's Q_IDENTIFY for `ver = 0`, TSO completion granularity, ENCAP on CSUM_PARTIAL | UNVERIFIABLE | no firmware source in these trees |

### Corrections and additions

**C1 (changes the stage 2 design). The TSO template is NOT prepared when the frame carries an in-packet
VLAN tag.** `ionic_tx_tcp_pseudo_csum()` tests `skb->protocol` against `ETH_P_IP`/`ETH_P_IPV6` and does
nothing for anything else (`K74:ionic_txrx.c:1378-1396`, same at `K726`). When the device does not grant
`IONIC_ETH_HW_VLAN_TX_TAG` (the recommended mask does not), the core pushes the tag into the packet
before the driver sees it: `validate_xmit_vlan()` -> `__vlan_hwaccel_push_inside()` ->
`vlan_insert_tag_set_proto()`, which sets `skb->protocol = vlan_proto`
(`C7215:net/core/dev.c:3981-3987,4089-4090`; `C7215:include/linux/if_vlan.h:488-496,530-537`). TSO
survives for a single tag: `netdev->vlan_features` inherits the TSO bits (`K74:ionic_lif.c:1643`),
`netif_skb_features()` intersects with `vlan_features` (`C7215:net/core/dev.c:3920-3924`), and
`vlan_features_check()` strips offloads only for multi-tagged frames
(`C7215:include/linux/if_vlan.h:803-818`). So a TCP send over a VLAN device on the guest's ionic port
reaches the device as opcode TSO with `hdr_len` including the 4-byte tag and with the stack's values
untouched: IPv4 `check` valid for the super-packet (non-zero), IPv6 `payload_len` as the stack left it,
and TCP `check` = pseudo-header sum **including the super-packet's TCP length**. A device that adds the
per-segment TCP length to that seed emits wrong TCP checksums on every such segment.

Consequence for the emulator: do not rely on the seed for TSO. Since the segmenter must parse L3/L4
anyway, zero the IPv4 `check` and TCP `check` fields in each segment and compute both from scratch
(full pseudo-header from the parsed addresses). That is correct for prepared and unprepared templates
alike. Section 3.5 step 6 and stage 2 step 3 should be read with this change. Add a stage 2 test: TCP
bulk transfer over an 802.1Q sub-interface in the guest with TSO on. (CSUM_PARTIAL is unaffected: its
seed and offsets come from the stack, not from this helper.) Granting VLAN_TX_TAG instead would avoid
the path only for hwaccel tags, at the cost of implementing insertion.

**C2 (refines open question 4).** `skb_linearize()` on an unreadable skb fails cleanly:
`__pskb_pull_tail()` returns NULL for `!skb_frags_readable()` (`C7215:net/core/skbuff.c:2886-2887`), so
`ionic_tx_descs_needed()` returns an error and `ionic_start_xmit()` drops the skb
(`K74:ionic_txrx.c:1773-1778,1841-1843,1860`). It is a drop, not a crash, and it needs more than 15 frags in
a non-GSO skb (at most one MTU of data) or more than 16 buffers in one MSS of a TSO skb; with page-sized
devmem chunks that requires many tiny `sendmsg()` calls coalesced into one MSS. A driver-series item
(count as a drop or split), not a blocker.

**C3 (addition; answers "is devmem TX on ionic a driver-only change": no).** The device grant is
load-bearing in the core, not only for `ethtool -k`:

- `tcp_sendmsg_locked()` returns `-EINVAL` for a dmabuf send unless `sk_route_caps & NETIF_F_SG`
  (`C7215:net/ipv4/tcp.c:1153-1175`); TCP sockets get SG|HW_CSUM in `sk_route_caps` through software GSO
  regardless of the device (`C7215:net/core/sock.c:2648-2662`), so the send itself succeeds.
- At xmit, a device without `NETIF_F_SG` makes `validate_xmit_skb()` linearize, which fails on unreadable
  frags; a device without `NETIF_F_HW_CSUM` makes it call `skb_checksum_help()`, which returns `-EFAULT`
  for unreadable frags (`C7215:net/core/dev.c:3624-3626,4106-4125`). Either way every data skb is dropped.
- So against today's emulator (SETATTR features = 0) devmem TX cannot pass traffic whatever the driver
  does, unless the driver ignored the negotiated mask. The emulator must grant `TX_SG|TX_CSUM`, answer
  TXQ v1/256, fix `len`, and fill CSUM_PARTIAL (stage 1).
- Stage 1 without TSO is INFERRED sufficient: the core then software-segments GSO skbs by frag
  reference, and `skb->unreadable` is in the copied header group
  (`C7215:include/linux/skbuff.h:970,1033,1089`). Not traced end to end through `skb_segment()`; if it
  fails, stage 2 (TSO) becomes required for devmem TX.
- `C7215` already has `enum netmem_tx_mode` (`NETMEM_TX_NONE/DMA/NO_DMA`,
  `C7215:include/linux/netdevice.h:1811`, checked at `C7215:net/core/dev.c:4048-4075`), so the backport
  sets a mode, not a bool. No ionic file at v7.4-rc3 mentions netmem (grep).

**C4 (minor).** `ionic_tx_descs_needed()` models the linear head as exactly `hdrlen` bytes
(`K74:ionic_txrx.c:1743`), true for TCP/devmem skbs. The cited `C7215:net/core/dev.c:3981-3988` and
`3990-3993` are off by a line or two (`3981-3987`, `3989-3993`); content is as described.
