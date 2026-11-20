# ionic RX header split + RX checksum: PoC device interface (frozen)

Normative for the emulator (`/var/tmp/ionic-devmem/rocm-ernic-rx`) and the driver
(`/var/tmp/ionic-devmem/linux-rx`). Written 2026-11-18 from `rx-design.md` (its Verification
pass wins), `linux-rx/.../ionic_if.h` (v7.4-rc3, unmodified) and the emulator RX tree
(`src/ionic/ionic_eth_emu.c`, `ionic_eth_steer.c`, `ionic_profile.c`). No source was changed.
"MUST/MUST NOT/MAY" are requirements; rule ids (`I`, `Q`, `C`, `S`, `P`, `K`, `M`, `T`, `D`)
are what tests cite. Where this file and `rx-design.md` section 4.2 differ, this file wins;
the differences are listed in section 9.

## 0. Status: PoC interface, not a firmware commitment

Every name and number below marked NEW was invented by this project so the emulator can stand
in for firmware that does not exist. Nothing here was assigned or reviewed by the Pensando/AMD
firmware team. The NEW values MUST NOT be sent upstream as an ionic device interface, and the
driver MUST keep every use of them behind the `IONIC_RXQ_F_HDS` identity bit so that it
behaves exactly as today on real hardware. Section 9 lists what real firmware may do
differently.

## 1. Definitions (ionic_if.h style)

All multi-byte fields are little-endian unless stated. Existing values are quoted for
reference; NEW marks additions.

```c
/* existing, enum ionic_q_feature */
	IONIC_Q_F_2X_CQ_DESC		= BIT_ULL(5),	/* 0x0000000000000020 */

/* enum ionic_rxq_feature: per-queue-type features, bits 16 and up */
	IONIC_RXQ_F_HWSTAMP		= BIT_ULL(16),	/* existing; never offered here */
	IONIC_RXQ_F_HDS			= BIT_ULL(17),	/* NEW 0x0000000000020000 */

/* The only two legal Q_INIT(RXQ).features values in header-split mode */
#define IONIC_RXQ_FEATURES_NONE		0x0ull
#define IONIC_RXQ_FEATURES_HDS		(IONIC_Q_F_2X_CQ_DESC | IONIC_RXQ_F_HDS) /* 0x20020 */

/**
 * struct ionic_rxq_comp_ext - NEW: leading half of a 2X Rx completion
 * @hds_hdr_len: Frame bytes the device wrote to the descriptor's own buffer
 *               (ionic_rxq_desc.addr), on every completion of an HDS queue
 * @hds_flags:   IONIC_RXQ_COMP_HDS_F_*
 * @rsvd:        zero
 * @hwstamp:     zero (IONIC_RXQ_F_HWSTAMP is not offered; the offset is the
 *               existing IONIC_HWSTAMP_CQ_NEGOFFSET = 8 before the base comp)
 */
struct ionic_rxq_comp_ext {	/* offset in the 32-byte CQ entry */
	__le16 hds_hdr_len;	/*  0, 2 bytes */
	u8     hds_flags;	/*  2, 1 byte  */
	u8     rsvd[5];		/*  3, 5 bytes */
	__le64 hwstamp;		/*  8, 8 bytes */
};				/* 16 bytes; struct ionic_rxq_comp follows at 16 */

#define IONIC_HDS_CQ_NEGOFFSET		16	/* NEW: ext = (void *)comp - 16 */
#define IONIC_RXQ_COMP_HDS_F_SPLIT	0x01	/* NEW: buffer 0 = exactly Eth..TCP headers */
#define IONIC_RXQ_COMP_HDS_F_HBO	0x02	/* NEW: headers > buffer 0; not split */
/* hds_flags bits 0xfc: reserved, zero */
```

Existing structures used unchanged (offsets for reference):

```
union ionic_q_identity   version u8 @0, supported u8 @1, rsvd[6] @2, features le64 @8,
                         desc_sz le16 @16, comp_sz le16 @18, sg_desc_sz le16 @20,
                         max_sg_elems le16 @22, sg_desc_stride le16 @24
struct ionic_q_init_cmd  opcode @0, lif_index le16 @2, type @4, ver @5, index le32 @8,
  (packed, 64 bytes)     pid le16 @12, intr_index le16 @14, flags le16 @16, cos @18,
                         ring_size @19, ring_base le64 @20, cq_ring_base le64 @28,
                         sg_ring_base le64 @36, rsvd2[12] @44, features le64 @56
struct ionic_rxq_desc    opcode u8 @0 (SIMPLE 0, SG 1), rsvd[5], len le16 @6, addr le64 @8
struct ionic_rxq_sg_elem addr le64 @0, len le16 @8, rsvd; 8 per descriptor, 128 bytes
struct ionic_rxq_comp    status u8 @0, num_sg_elems u8 @1, comp_index le16 @2,
  (16 bytes)             rss_hash le32 @4, csum @8 (2 bytes, rule K2), vlan_tci le16 @10,
                         len le16 @12, csum_flags u8 @14, pkt_type_color u8 @15
csum_flags               TCP_OK 0x01, TCP_BAD 0x02, UDP_OK 0x04, UDP_BAD 0x08,
                         IP_OK 0x10, IP_BAD 0x20, VLAN 0x40, CALC 0x80
LIF features             IONIC_ETH_HW_RX_CSUM = BIT(4)
```

No new LIF feature bit, descriptor opcode, Q_INIT field or admin command is defined. Header
split is negotiated per queue only (I1, Q1). No flow-steering definitions are part of this
interface (section 6).

## 2. The emulator mode

- **M1.** New profile section and key: `[rx]` / `header_split = off | on`. The built-in
  `compat` profile sets `header_split = off`, so the default is off. A new built-in profile
  `header-split` holds only `[rx]` + `header_split = on`; the server option that turns the
  mode on is therefore `--nic-profile header-split` (or a profile file with the key). No
  dedicated command-line flag.
- **M2.** Mode off: the device is today's device. Nothing in sections 3 to 5 applies: LIF
  features, `Q_IDENTIFY`, `Q_INIT` (which ignores `features`), buffer placement and every
  completion byte are identical to the tree without this work. The existing golden identity
  test (`tests/golden/ionic_compat_identity.h`) MUST pass unchanged.
- **M3.** Mode on changes exactly three things: RX checksum reporting on every RX queue
  (section 5), the RXQ identity (I1), and `Q_INIT(RXQ)` feature handling (Q1 to Q4). The mode
  is fixed for the life of the server process.
- **M4.** Mode on, guest that never sets `IONIC_RXQ_F_HDS` (a stock driver): placement, CQ
  entry size (16) and all completion bytes other than `csum` (@8, @9) and `csum_flags` (@14)
  are identical to mode off.

## 3. Identification and request

- **I1.** Mode on: `Q_IDENTIFY` for `IONIC_QTYPE_RXQ` returns `features = 0x20020`
  (`IONIC_Q_F_2X_CQ_DESC | IONIC_RXQ_F_HDS`). Every other identity field is as today:
  `version 0`, `supported 0x01`, `desc_sz 16`, `comp_sz 16` (the base size; the driver doubles
  it itself, as it does for the hwstamp queue), `sg_desc_sz 128`, `max_sg_elems 8`,
  `sg_desc_stride 8`. TXQ, ADMINQ and NOTIFYQ identities are unchanged (`features = 0`).
- **I2.** LIF features granted, in both the LIF identity (`eth.config.features`, identity
  byte 0xa8) and the `LIF_SETATTR(FEATURES)` completion mask: mode off = today's
  `TX_SG | TX_CSUM | TSO | TSO_IPV6 | TSO_ECN` (0x7a0); mode on = that plus
  `IONIC_ETH_HW_RX_CSUM` (0x7b0). Not granted in either mode: `RX_HASH`, `RX_SG`,
  `VLAN_RX_STRIP`, `VLAN_RX_FILTER`, `TIMESTAMP`, any flow-steering bit.
- **Q1.** The driver requests header split per RX queue with
  `Q_INIT.features = 0x20020` and `Q_INIT.flags & IONIC_QINIT_F_SG` and a non-zero
  `sg_ring_base`. It MUST do so only if I1's identity had `IONIC_RXQ_F_HDS`.
- **Q2.** Mode on, `Q_INIT` with `type == RXQ`: `features == 0` gives a non-HDS queue;
  `features == 0x20020` with `F_SG` and `sg_ring_base != 0` gives an HDS queue. Anything else
  is refused and the queue's previous state is left untouched: a bit outside `0x20020` gives
  `IONIC_RC_ENOSUPP` (19); `2X_CQ_DESC` without `HDS`, `HDS` without `2X_CQ_DESC`, or `HDS`
  without `F_SG` or with `sg_ring_base == 0` gives `IONIC_RC_EINVAL` (14). The existing checks
  (qtype, index, ring size) run first, with their existing codes.
- **Q3.** Completion size: a non-HDS queue's CQ entry is 16 bytes (`struct ionic_rxq_comp`),
  at `cq_ring_base + i * 16`. An HDS queue's CQ entry is 32 bytes, at `cq_ring_base + i * 32`:
  `struct ionic_rxq_comp_ext` at bytes 0..15, `struct ionic_rxq_comp` at bytes 16..31, so the
  colour bit is bit 7 of byte 31. The CQ has the same number of entries as the descriptor
  ring. The device writes all 32 bytes of every entry, zero where not specified.
- **Q4.** HDS is a property of one queue from its `Q_INIT` until its next `Q_INIT`. Queues of
  one LIF may differ. `Q_CONTROL`, `LIF_SETATTR` and the doorbell protocol are unchanged.
  Re-`Q_INIT` of an index replaces all its state (ring addresses, entry size, HDS, indices,
  colour 1), as today. The emulator processes commands and RX on one thread, so after
  `Q_CONTROL(DISABLE)` or `Q_INIT` completes it writes nothing more to buffers posted on the
  old ring.
- **Q5.** `Q_INIT.features` on other queue types is ignored, as today.

## 4. Placement and the extension fields (HDS queue)

Terms. `len` = frame length (no FCS). `buf0` = the descriptor's own buffer (`desc.addr`,
`desc.len`). `l3` = offset of the IP header: 14 plus 4 for each of at most two leading VLAN
tags (EtherType 0x8100 or 0x88a8); VLAN tags are never stripped. "Prefix fill" = today's rule:
write frame bytes to `buf0` up to `desc.len`, then to SG elements 0, 1, ... each filled to its
`len` before the next, stopping at the first zero-length element.

- **P1 (parse).** A frame is *splittable* iff all of these hold:
  1. `len >= l3 + 20` and the EtherType after the tags is 0x0800 or 0x86dd.
  2. IPv4: version 4; `ihl = (byte0 & 0xf) * 4`, `20 <= ihl <= len - l3`; not a fragment
     (`(be16 @6) & 0x3fff == 0`); protocol 6; header checksum over `ihl` bytes verifies;
     `l4 = l3 + ihl`; `end = l3 + total_length`.
     IPv6: `len >= l3 + 40`; version 6; next header (byte 6) is 6, i.e. no extension header
     of any kind; `l4 = l3 + 40`; `end = l3 + 40 + payload_length`.
  3. `end == len` exactly. A frame with trailing bytes (Ethernet padding) or cut short is not
     splittable.
  4. `len >= l4 + 20`; `doff = (byte 12 of TCP >> 4) * 4`, `20 <= doff <= len - l4`.
  5. The TCP checksum (pseudo-header plus bytes `[l4, len)`) verifies.
  Then `hdr_len = l4 + doff` (so it includes VLAN tags, IPv4 options and TCP options) and the
  payload is bytes `[hdr_len, len)`, possibly empty.
- **P2 (split).** For a splittable frame on an HDS queue whose head descriptor has opcode `SG`,
  with `hdr_len <= desc.len` and the SG elements up to the first zero-length one having total
  room `>= len - hdr_len`: bytes `[0, hdr_len)` go to `buf0`; bytes `[hdr_len, len)` go to SG
  elements 0, 1, ... each filled to its `len` before the next. Nothing past `hdr_len` is
  written to `buf0`. Completion: `num_sg_elems` = elements written (0 when the payload is
  empty), `len` = whole frame, `hds_hdr_len = hdr_len`, `hds_flags = SPLIT`.
- **P3 (not split).** Every other frame on an HDS queue is prefix-filled, byte for byte as on a
  non-HDS queue. Completion: `num_sg_elems` and `len` as today,
  `hds_hdr_len = min(len, desc.len)`, `SPLIT` clear. `HBO` is set iff the frame is splittable,
  the opcode is `SG`, and `hdr_len > desc.len`; otherwise `hds_flags = 0`.
- **P4 (invariants, all completions of an HDS queue).** `hds_hdr_len` is the number of frame
  bytes in `buf0`. The frame is `buf0[0, hds_hdr_len)` followed by `num_sg_elems` elements,
  each full except the last. `SPLIT` and `HBO` are never both set. `SPLIT` implies
  `csum_flags & TCP_OK`, and for IPv4 also `IP_OK` (C3 holds by construction).
- **P5 (drop).** A frame the posted buffers cannot hold by prefix fill (`buf0` plus elements
  `< len`, or `len > desc.len` with opcode `SIMPLE`) is dropped as today: nothing is written,
  no completion, the descriptor stays at the head, the existing drop counter moves. A bad
  opcode or unreadable descriptor/SG list is handled as today, with one difference on an HDS
  queue: a splittable frame **with payload** has its SG list read even when the whole frame
  would fit `buf0` (its payload belongs in the elements), so an unreadable list drops it
  (`rx_desc_fetch_error`) where a non-HDS queue would have delivered it without looking at
  the list. A splittable frame with no payload reads no list. (Decided in the emulator,
  2026-11-18; moved here from the appendix in the review round. `docs/ionic.rst`, "What is
  split", and the comment on `eth_rx_bufs()` say the same.)
- **P6.** `status` is 0 on every completion the emulator writes; it never writes status 2.

Case table (HDS queue, opcode `SG`, `desc.len = 256`, elements of 4096; `Pn` = rule):

| # | frame | result | `hds_hdr_len` | `hds_flags` | `num_sg_elems` |
| --- | --- | --- | --- | --- | --- |
| 1 | TCP/IPv4, no options, payload 1448, len 1502 | split at 54 (P2) | 54 | SPLIT | 1 |
| 2 | TCP/IPv4, 12 bytes TCP options, payload 16, len 82 | split at 66 | 66 | SPLIT | 1 |
| 3 | as 2 with one 802.1Q tag (or two tags: +8) | split at 70 (74) | 70 (74) | SPLIT | 1 |
| 4 | TCP/IPv4 with IHL 6..15 (IP options) | split at `l3 + ihl + doff` | that | SPLIT | per payload |
| 5 | TCP/IPv6, no extension header, payload n > 0 | split at `l3 + 40 + doff` | that | SPLIT | per payload |
| 6 | TCP SYN / pure ACK with `end == len` (e.g. 74-byte SYN) | split, empty payload | len | SPLIT | 0 |
| 7 | padded frame: pure ACK or 1..5 byte payload padded to 60 | not split (P1.3) | 60 | 0 | 0 |
| 8 | TCP/IPv6 behind any extension header (incl. fragment 44) | not split (P1.2) | min(len,256) | 0 | prefix |
| 9 | IPv4 fragment, first or later | not split | min(len,256) | 0 | prefix |
| 10 | UDP, ICMP, SCTP, any non-TCP IP | not split | min(len,256) | 0 | prefix |
| 11 | ARP, other EtherTypes, three or more VLAN tags | not split | min(len,256) | 0 | prefix |
| 12 | runt or truncated: `len < l3 + 20`, short TCP header, `end > len` | not split | min(len,256) | 0 | prefix |
| 13 | bad IPv4 header checksum or bad TCP checksum | not split | min(len,256) | 0 | prefix |
| 14 | splittable but `hdr_len > desc.len` (e.g. `desc.len = 64`, case 2) | not split | `desc.len` | HBO | prefix |
| 15 | splittable, payload > SG room but frame fits by prefix fill | not split | min(len,256) | 0 | prefix |
| 16 | any frame, opcode `SIMPLE` (fits) | not split | len | 0 | 0 |
| 17 | frame larger than all posted room | dropped (P5) | no completion | | |

"prefix" = the number of SG elements prefix fill uses, as today.

## 5. RX checksum reporting (mode on, every RX queue, HDS or not)

- **K1.** The fields are written on every RX completion regardless of what the driver asked
  for in `LIF_SETATTR(FEATURES)`; I2's `RX_CSUM` grant is what entitles the driver to use
  them. Mode off: `csum = 0`, `csum_flags = 0`, as today.
- **K2 (`csum`, `CALC`).** If `len >= 14`: `csum_flags |= CALC (0x80)` and `csum` is the
  16-bit ones-complement sum (RFC 1071, end-around carry, **not** inverted) of frame bytes
  `[14, len)` taken as big-endian 16-bit words, an odd final byte padded with a zero low byte.
  It covers VLAN tags and padding, and is independent of where the bytes were placed. Storage:
  completion byte 8 = high byte of that sum, byte 9 = low byte (i.e. network order, although
  `ionic_if.h` types the field `__le16`). This is what makes the driver's
  `skb->csum = le16_to_cpu(comp->csum)` equal the kernel's native `__wsum` on a little-endian
  guest; big-endian guests are out of scope. If `len < 14`: `csum = 0`, `csum_flags = 0`.
- **K3 (`IP_OK`/`IP_BAD`).** IPv4 header found (`len >= l3 + 20`, version 4,
  `20 <= ihl <= len - l3`): `IP_OK (0x10)` if the header checksum verifies, else
  `IP_BAD (0x20)`. Never set for IPv6 or non-IP.
- **K4 (L4 flags).** Evaluated only for IPv4 with `IP_OK`, not a fragment, or IPv6 whose next
  header is directly TCP or UDP; and only if `l3 + total_length <= len` (IPv6:
  `l3 + 40 + payload_length <= len`). The L4 length `n` is taken from the IP header, so
  padding is excluded.
  TCP (protocol 6), `n >= 20`: `TCP_OK (0x01)` if the checksum over pseudo-header and `n`
  bytes verifies, else `TCP_BAD (0x02)`.
  UDP (17), `n >= 8`, UDP length field `== n`, checksum field non-zero: `UDP_OK (0x04)` or
  `UDP_BAD (0x08)` likewise. A zero UDP checksum field sets neither flag.
  In every other case no L4 flag is set (the stack verifies through `CALC`).
- **K5.** `VLAN (0x40)` is never set; `vlan_tci`, `rss_hash` and the packet-type bits of
  `pkt_type_color` stay 0, as today.

Resulting `csum_flags` for good packets: TCP/IPv4 `0x91`, UDP/IPv4 `0x94`, TCP/IPv6 `0x81`,
UDP/IPv6 `0x84`, ARP and other non-IP `0x80`, IPv4 fragment or ICMP `0x90`.

## 6. Driver obligations (what the device relies on)

- **D1.** Only use NEW definitions when I1 offered `IONIC_RXQ_F_HDS`; request per Q1; size the
  CQ at 32 bytes per entry and read the base completion at entry offset 16 (the existing 2X
  code in `__ionic_rx_service()` already does) and the extension at offset 0.
- **D2.** On an HDS queue post opcode `SG` with `buf0` readable host memory of at least 256
  bytes (`desc.len` is the header capacity; nothing else conveys it) and payload buffers in SG
  elements. `desc.len < 54` makes every TCP frame HBO.
- **D3.** Build the skb from P4 alone: copy `hds_hdr_len` bytes from `buf0` to the linear
  area, attach `num_sg_elems` frags. Do not re-parse to find the split point.
- **D4.** Checksum: with `NETIF_F_RXCSUM`, a completion with `TCP_OK` or `UDP_OK` SHOULD
  become `CHECKSUM_UNNECESSARY`; otherwise `CALC` gives `CHECKSUM_COMPLETE` as today. On an
  HDS queue the driver MUST NOT hand the stack `CHECKSUM_NONE` or `CHECKSUM_COMPLETE` with
  unreadable frags; `SPLIT` guarantees `TCP_OK` (P4), so `SPLIT` completions always qualify
  for `CHECKSUM_UNNECESSARY`.
- **D5.** A completion with `SPLIT` clear and `num_sg_elems > 0` carries non-header bytes in
  payload buffers. If those buffers are unreadable (devmem bound) the driver SHOULD drop it and
  count it; the device will not avoid producing it (cases 8 to 15).

## 7. Steering for the test

The emulator's RX queue selection is unchanged by this interface, in both modes:

- **T1.** `queue = active[h % n]`, where `active` is the ascending list of RX queue indices
  that are initialised (`Q_INIT`) and enabled (`Q_CONTROL` ENABLE) at the moment the frame is
  delivered, and `n` its length (`ionic_eth_emu_rx_active()`, `ionic_eth_emu_rx_steer()`). With
  `ethtool -L IF combined N` and all queues up, `active = [0..N-1]`.
- **T2.** `h` is unkeyed 32-bit FNV-1a (offset basis `0x811c9dc5`, prime `0x01000193`, per
  byte `h ^= b; h *= prime` mod 2^32) over, in wire order (`ionic_eth_flow_hash()`):
  IPv4: source address (4), destination address (4), protocol (1), then source port (2) and
  destination port (2) iff the protocol is 6, 17 or 132, the frame is not a fragment and holds
  those 4 bytes. IPv6: source (16), destination (16), next-header byte (1), then the ports
  under the same protocol test on the fixed header's next header (extension headers are not
  walked). Anything else: the 12 MAC bytes. Up to two VLAN tags are skipped. The guest's RSS
  key, indirection table and `RX_FILTER_ADD` are ignored.
- **T3.** So for TCP/IPv4 the input is 13 bytes and a sender picks its source port:

  ```python
  def fnv1a(b, h=0x811c9dc5):
      for x in b:
          h = ((h ^ x) * 0x01000193) & 0xffffffff
      return h
  def rxq(src, dst, sport, dport, n):      # src/dst: 4 raw bytes each
      return fnv1a(src + dst + bytes([6]) + sport.to_bytes(2, "big")
                   + dport.to_bytes(2, "big")) % n
  ```

  Checked against the C code (`ionic_eth_steer.c` compiled standalone): 10.0.0.1:40000 ->
  10.0.0.2:5201 gives `h = 0xab8b421d`; with `n = 4`, source ports 40003, 40000, 40001, 40002
  land on queues 0, 1, 2, 3. The reverse direction (ACKs to the sender) hashes differently.
- **T4.** Limits a test MUST respect: the port choice does not keep other flows off the queue
  (ARP, neighbour discovery, SSH, the RDMA driver's traffic hash wherever they hash); `n` changes while queues
  are being restarted, e.g. during a bind whose queue-op stub disables every queue, so frames
  in that window may land elsewhere; a `--p4` pipeline may consume a frame before steering.
- **T5 (scope).** A flow-steer filter (`RX_FILTER_ADD` match type, `NETIF_F_NTUPLE`) and RSS
  indirection are **OUT** of first-light scope. Not trivial: the emulator keeps no filter
  state, answers every `RX_FILTER_ADD` with `filter_id` 0, and has no `RX_FILTER_DEL`
  handling; the driver has no `rxnfc` ops. First light uses `ethtool -L IF combined 1`, or two
  queues with T3's port choice, and `ncdevmem -n`. The sketch in `rx-design.md` 4.2
  (`IONIC_RX_FILTER_STEER_FLOW`, LIF `BIT(22)`) is not frozen and MUST NOT be implemented
  against this document.

## 8. Worked examples

Frames from `/var/tmp/ionic-devmem/test/scratch/rx-interface/ex.py` (checksums computed
there; `a.bin`, `b.bin` beside it). Flow 10.0.0.1:40000 -> 10.0.0.2:5201. Queue colour 1.

**Example A, split.** 82 bytes: Ethernet, IPv4 (IHL 5, total length 68), TCP with 12 option
bytes (`doff = 32`), 16 payload bytes `41..50`. `l3 = 14`, `l4 = 34`, `hdr_len = 66`,
`end = 82 == len`. IPv4 checksum `0x147e` and TCP checksum `0x62e3` verify.

```
0000  02 00 00 00 00 0b 02 00 00 00 00 0a 08 00 45 00
0010  00 44 12 34 40 00 40 06 14 7e 0a 00 00 01 0a 00
0020  00 02 9c 40 14 51 00 00 03 e8 00 00 07 d0 80 18
0030  ff ff 62 e3 00 00 01 01 08 0a 00 00 00 64 00 00
0040  00 c8|41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e      "|" = byte 66, the split
0050  4f 50
```

HDS queue, head descriptor index 5: opcode `SG`, `len 256`, SG element 0 `len 4096`,
element 1 `len 0`. The device writes frame bytes `[0, 66)` to `desc.addr` (bytes 66..255 of
that buffer untouched), bytes `[66, 82)` = `41 .. 50` to element 0's address, and this
32-byte CQ entry (sum of `[14, 82)` = `0xebc6`):

```
00  42 00 01 00 00 00 00 00  00 00 00 00 00 00 00 00   hds_hdr_len 66, SPLIT, hwstamp 0
10  00 01 05 00 00 00 00 00  eb c6 00 00 52 00 91 80   status 0, num_sg 1, comp_index 5,
                                                       csum, len 82, CALC|IP_OK|TCP_OK, colour
```

The same frame on a non-HDS queue (mode on) with `buf0` of 256: all 82 bytes in `buf0`,
16-byte entry `00 00 05 00 00 00 00 00 eb c6 00 00 52 00 91 80`. Mode off: the same with
`00 00` at bytes 8..9 and `00` at byte 14.

**Example B, not split (padded).** 60 bytes: same flow, no TCP options, 1 payload byte `5a`
at offset 54, total length 41, so `end = 55 != len = 60` (5 pad bytes): P1.3 fails.

```
0000  02 00 00 00 00 0b 02 00 00 00 00 0a 08 00 45 00
0010  00 29 12 34 40 00 40 06 14 99 0a 00 00 01 0a 00
0020  00 02 9c 40 14 51 00 00 03 f8 00 00 07 d0 50 18
0030  ff ff 85 6f 00 00 5a 00 00 00 00 00
```

HDS queue, head descriptor index 6, same buffers: all 60 bytes go to `desc.addr`, no SG
element is written. The TCP checksum is verified over the 21 bytes the IP header names, so
`TCP_OK` is still set; the sum of `[14, 60)` is `0xebe1`:

```
00  3c 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00   hds_hdr_len 60, no flags
10  00 00 06 00 00 00 00 00  eb e1 00 00 3c 00 91 80   num_sg 0, comp_index 6, len 60
```

## 9. What real firmware might do differently

1. Whether RX header split exists at all, and its negotiation: a LIF feature bit, a queue
   version, a different queue feature bit than `BIT_ULL(17)`, or a `Q_INIT` field carrying a
   header-buffer size or a separate header ring.
2. Where the header length is reported: a spare or redefined field, a different extension
   layout, co-existence with `hwstamp`, a 4X completion; whether `comp_sz` in the identity
   already reports the doubled size.
3. Whether a `buf0` smaller than the frame, or below some minimum, is accepted, and the fill
   order across `buf0` and SG elements (the "prefix fill" rule is not written in `ionic_if.h`).
4. What is split: UDP, IPv6 with extension headers, tunnelled packets (inner or outer),
   padded frames, bad-checksum frames, pure ACKs flagged or not; a split threshold
   (`hds-thresh`); behaviour on header overflow (truncate, drop, or flag); status 2.
5. Checksum: whether `CALC` is set for every frame, what `csum` covers with VLAN stripping,
   padding and odd lengths, its byte order, whether `_OK` flags accompany `CALC`, zero-checksum
   UDP, and whether reporting follows the negotiated `RX_CSUM` (here it does not, K1).
6. `pkt_type`, `rss_hash` and VLAN stripping, which real devices fill and this one leaves 0.
7. Whether per-queue feature sets may differ within a LIF, whether one RX queue may be
   re-`Q_INIT`ed while the LIF is up, and whether `Q_CONTROL(DISABLE)` is synchronous (Q4 is an
   emulator property).
8. Steering: real devices use Toeplitz RSS with the guest's key and table; none offers the
   unkeyed FNV hash a test can precompute. Flow filters and their priority over RSS.
9. Error codes for a bad `Q_INIT.features` (Q2), and whether unknown bits are ignored.
10. Limits: 8 SG elements of at most 65535 bytes; CMB RX rings; DMA to device memory.

Differences from the `rx-design.md` 4.2 sketch, on purpose: TCP only (UDP is never split);
padded, truncated and bad-checksum frames are never split (Verification pass); `SPLIT` implies
`TCP_OK`; a split whose payload does not fit the SG elements falls back to prefix fill;
`2X_CQ_DESC` alone is refused; `Q_INIT` error codes are fixed; the mode key gates RX checksum
as well as header split; flow steering is removed from the interface.

## 10. What was and was not checked

- Read: `rx-design.md` in full; `ionic_if.h` definitions cited in section 1; the emulator's
  `Q_IDENTIFY`, `Q_INIT`, `Q_CONTROL`, `eth_rx_bufs()`, `eth_rx_deliver()`, steering, profile
  loader and built-in profiles in the RX tree (which has the TX work but no RX change yet).
- Run: `ex.py` (frames, checksums, FNV values) and `ionic_eth_steer.c` compiled standalone
  with a 10-line `main` (`m.c`, same directory): both example frames hash to `0xab8b421d`,
  matching the Python. Nothing else was built or run; no guest was booted.
- Not verified: K2's byte order against a running kernel (derived, as in `rx-design.md` 4.3;
  a wrong order shows as "hw csum failure" in the guest log and MUST be the first thing the
  mode-on smoke test checks with a stock driver); that `BIT_ULL(17)` collides with nothing out
  of tree; any real-firmware behaviour.

## Changes during implementation

- 2026-11-19, driver (`impl-driver-rx.md`): **no change to the device interface.** Driver
  choices a test should know about:
  - D4 is narrowed. The driver reports `CHECKSUM_UNNECESSARY` from `TCP_OK`/`UDP_OK` only on
    queues it initialised with `IONIC_RXQ_F_HDS`; on every other queue it keeps
    `CHECKSUM_COMPLETE` from `CALC`, as a stock driver does. So with mode on and
    `tcp-data-split off`, K2's byte order is still exercised by TCP and UDP.
  - D2: `buf0` is exactly 256 bytes; the SG elements are whole pages and together hold
    `mtu + 18` bytes (a whole frame), every RX queue of the netdev is initialised with
    `0x20020` once tcp-data-split is on.
  - D5 is implemented as a drop (`rx_N_unreadable_drop`). The driver also drops, as
    `rx_N_dropped`, an HDS completion with `hds_hdr_len < 14`, `> 256` or `> len`, with
    `num_sg_elems` above what it posted, or whose length does not fit the elements used.

- 2026-11-18, emulator (`impl-emulator-rx.md`): **no rule changed; nothing was unimplementable.**
  Five points the text left open, as the emulator decides them (each has a unit-test row):
  - Q2 order: a bit outside `0x20020` is tested first, so `0x30020` without an SG ring is
    `ENOSUPP` (19), not `EINVAL`. Refusals log one error line; mode off never reads `features`.
  - K3/K4: an IPv4 `total_length` smaller than the header length (`ihl`) gets `IP_OK` (if the
    header checksum is good) and no L4 flag; it is never split.
  - P2/P5: for a splittable frame *with payload* the SG list is always read, so an unreadable
    list drops the frame (`rx_desc_fetch_error`) even where the whole frame would have fitted
    `buf0` by prefix fill. A splittable frame with no payload (SYN, unpadded ACK) reads no list.
  - P3/P5: `HBO` is only ever seen on a completion; an HBO frame the buffers cannot hold by
    prefix fill is dropped like any other (case 17), e.g. `desc.len 53` + one 1448-byte element
    for the 1502-byte frame of case 1.
  - K2 for a frame of fewer than 14 bytes: delivered with `csum 0`, `csum_flags 0`, and on an
    HDS queue `hds_hdr_len = len`, `hds_flags 0` (the driver's `hds_hdr_len < 14` drop covers it).
  Not guest-visible, but new: `--print-profile` prints the `[rx]` section and the profile hash
  changes in both modes; with the mode on the server logs
  `NIC Rx header split: on (a prototype no ionic firmware has), with Rx checksum reporting`.
  Still not verified against a kernel: K2's byte order (only derived and unit-tested as
  "byte 8 = high byte").

- 2026-11-18, integration run (`test-rx.md`): **no rule changed; emulator not modified.** What a
  kernel now confirms, and two things a reader of this file should know:
  - K2's byte order is right: a stock v7.4-rc3 ionic and the new driver with tcp-data-split off
    both take `CHECKSUM_COMPLETE` from it for TCP, UDP, IPv6 and VLAN traffic with no
    "hw csum failure" (`test-rx/evidence/01-*`, `21-*`, `60-*`).
  - I1/Q1-Q3/P2-P4/D1-D5 work against the driver: 32-byte completions, split at the end of the
    TCP header (linear 52 bytes after the Ethernet header for IPv4 with timestamps, 72 for
    IPv6, 56 behind one VLAN tag), `TCP_OK` on every split frame.
  - Case 7 (padded frame) is not reachable with guest traffic on the qemu-tool stack: the TAP
    path carries frames shorter than 60 bytes unpadded, so `end == len` holds and 1 to 5 byte
    TCP payloads **are** split even without TCP timestamps. The caveat in `impl-driver-rx.md`
    ("senders need TCP timestamps on") does not apply there. Only the emulator's unit test
    covers padding.
  - T1/T3 steering was used as written (`test-rx/scripts/rxq.py`): with two queues,
    192.168.100.12:40000 -> 192.168.100.11:5201 lands on queue 1 and :40001 on queue 0.
