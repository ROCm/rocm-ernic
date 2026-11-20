# Reference drivers: how upstream enables devmem/netmem TX

Research date 2026-11-18. Trees: `/var/tmp/ionic-devmem/linux` (v7.4-rc3, full
history), tags `v7.2.6`, `v7.2.15`. Read-only; nothing modified.

Citations are `path:line @tag`. Unless marked INFERRED, every statement is
VERIFIED by reading the source or `git show` of the named commit.
"First release" is from `git describe --contains`.

## 0. Decisive facts (short)

- Only **four** hardware drivers set `netmem_tx` in both v7.2.6 and v7.4-rc3:
  bnxt, gve (DQO-RDA only), mlx5e, fbnic; plus netkit (`NETMEM_TX_NO_DMA`).
  ice, idpf, tg3, virtio_net do **not** (no `netmem_tx` hit under `drivers/`
  other than those five). bnge (`bng_en`) uses the netmem unmap helpers but
  never sets `netmem_tx`.
- Every driver patch is tiny (15-40 lines) and does exactly two things:
  (a) make sure a net_iov frag is never passed to `dma_unmap_page()`;
  (b) set `netdev->netmem_tx`. Mapping needs no change because
  `skb_frag_dma_map()` already returns the dma-buf address for net_iov frags.
- Two idioms for (a):
  1. `netmem_dma_unmap_addr_set()` stores **0** as the unmap address for a
     net_iov, and `netmem_dma_unmap_page_attrs()` skips addr 0 (gve, bnxt,
     mlx5e, bnge).
  2. A driver-private "do not unmap" marker per ring slot (fbnic:
     `FBNIC_XMIT_NOUNMAP`).
- **ionic must use idiom 2 (or a variant)**: ionic reads
  `buf_info->dma_addr` back to fill the descriptor / SG elements
  (`ionic_txrx.c:1515,1643 @v7.4-rc3`), so zeroing the stored address with
  `netmem_dma_unmap_addr_set()` would program address 0 into the device.
- No driver added counters, no driver changed `ndo_features_check`, no driver
  gates `netmem_tx` on netdev features. gve gates on queue format; bnxt gates
  its TX-push (inline copy) fast path on `skb_frags_readable()`.
- No driver handles frag-count overflow for unreadable skbs:
  `skb_linearize()` fails for them (`__pskb_pull_tail()` returns NULL), so the
  skb is dropped. ionic has exactly that path
  (`ionic_tx_descs_needed()`), with `max_sg_elems` 8 or 15 against
  `MAX_SKB_FRAGS` 17.
- The driver-facing API (`netmem_tx` enum, `NETMEM_TX_DMA`,
  `netmem_dma_unmap_*`, `skb_frag_is_net_iov()`, `skb_frag_netmem()`,
  `skb_frags_readable()`, `skb_frag_dma_map()`) is **source-identical for a
  driver between v7.2.6, v7.2.15 and v7.4-rc3**. The backport needs no compat
  shims on x86-64.

## 1. Commit inventory

| sha | date | subject | first release |
|---|---|---|---|
| f3600c867c99 | 2025-03-06 | netmem: prevent TX of unreadable skbs | v6.14-rc7 |
| 383faec0fd64 | 2025-05-08 | net: enable driver support for netmem TX | v6.16-rc1 |
| c32532670cec | 2025-05-08 | gve: add netmem TX support to GVE DQO-RDA mode | v6.16-rc1 |
| ae28cb114727 | 2025-05-08 | net: check for driver support in netmem TX | v6.16-rc1 |
| 5a842c288cfa | 2025-06-16 | net/mlx5e: Add TX support for netmems | v6.17-rc1 |
| 4672aec56d2e | 2025-06-19 | netmem: fix skb_frag_address_safe with unreadable skbs | v6.17-rc1 |
| 4922ca773d9d | 2025-06-19 | eth: bnxt: add netmem TX support | v6.17-rc1 |
| 870bc1aaa0f9 | 2025-07-16 | net/mlx5e: TX, Fix dma unmapping for devmem tx | v6.17-rc1 |
| b127e355f1af | 2025-09-16 | eth: fbnic: support devmem Tx | v6.18-rc1 |
| bd5ad9c052c8 | 2026-01-29 | bng_en: Add TX support (helpers only, no netmem_tx) | v7.0-rc1 |
| 7d3ab852dcd8 | 2026-05-14 | net: convert netmem_tx flag to enum | v7.2-rc1 |
| 6ce2bb048055 | 2026-05-14 | net: netkit: declare NETMEM_TX_NO_DMA mode | v7.2-rc1 |
| 1abe839b34ae | 2026-05-14 | net: devmem: support TX over NETMEM_TX_NO_DMA devices | v7.2-rc1 |
| 5c729e6c71f1 | 2026-09-25 | net: devmem: decode DMA addresses for TX | v7.4-rc1 |

Found with `git log -S'netmem_tx'`, `-S'netmem_dma_unmap_addr_set'` and
`--grep` on netmem/devmem TX. The original core+gve commits were merged as
branch `device-memory-tcp-tx` (merge ac4d1baf97fd, Mina Almasry's cover
letter: "The series is relatively small, as the TX path for this feature
largely piggybacks on the existing MSG_ZEROCOPY implementation.").

## 2. Core commits

### 2.1 383faec0fd64 "net: enable driver support for netmem TX"

Message: "Drivers need to make sure not to pass netmem dma-addrs to the
dma-mapping API in order to support netmem TX. Add helpers and
netmem_dma_*() helpers that enables special handling of netmem dma-addrs
that drivers can use. Document in netmem.rst what drivers need to do to
support netmem TX."
Trailers: SoB Mina Almasry; Acked-by Stanislav Fomichev; Link
`20250508004830.4100853-7-almasrymina@google.com`; applied by Paolo Abeni.

Hunks:
- `include/linux/netdevice.h`: adds `unsigned long netmem_tx:1;` in
  `priv_flags_fast` and kdoc.
- `include/net/netmem.h`: adds the two helpers (unchanged since; current
  text at `include/net/netmem.h:447-464 @v7.4-rc3`,
  `:397-414 @v7.2.6` and `@v7.2.15`):

```c
#define netmem_dma_unmap_addr_set(NETMEM, PTR, ADDR_NAME, VAL)   \
	do {                                                     \
		if (!netmem_is_net_iov(NETMEM))                  \
			dma_unmap_addr_set(PTR, ADDR_NAME, VAL); \
		else                                             \
			dma_unmap_addr_set(PTR, ADDR_NAME, 0);   \
	} while (0)

static inline void netmem_dma_unmap_page_attrs(struct device *dev,
					       dma_addr_t addr, size_t size,
					       enum dma_data_direction dir,
					       unsigned long attrs)
{
	if (!addr)
		return;

	dma_unmap_page_attrs(dev, addr, size, dir, attrs);
}
```

- Docs: `netmem.rst` gains "Driver TX Requirements" (current text
  `Documentation/networking/netmem.rst:134-157 @v7.4-rc3`):
  "The Driver must not pass the netmem dma_addr to any of the dma-mapping
  APIs directly ... Helpers like netmem_dma_unmap_page_attrs() &
  netmem_dma_unmap_addr_set() should be used in lieu of
  dma_unmap_page[_attrs](), dma_unmap_addr_set()." and "Driver should declare
  support by setting `netdev->netmem_tx` to the appropriate mode".
  `netdev-features.rst:216-220 @v7.4-rc3` now says "This is not a netdev
  feature bit."

Note (VERIFIED `include/linux/dma-mapping.h:786-803 @v7.4-rc3`):
`dma_unmap_addr_set()` is a no-op without `CONFIG_NEED_DMA_MAP_STATE`, so
`netmem_dma_unmap_addr_set()` on a plain `dma_addr_t` field (as mlx5 does)
only stores anything when that option is set (x86 selects it,
`arch/x86/Kconfig:33`).

### 2.2 ae28cb114727 "net: check for driver support in netmem TX"

Message: "We should not enable netmem TX for drivers that don't declare
support. Check for driver netmem TX support during devmem TX binding and fail
if the driver does not have the functionality. Check for driver support in
validate_xmit_skb as well." Acked-by Stanislav Fomichev.

Hunks:
- `net/core/dev.c`: new `validate_xmit_unreadable_skb()` replacing the
  unconditional drop added by f3600c867c99. Current form
  (`net/core/dev.c:4169-4197 @v7.4-rc3`; `:3991-4019 @v7.2.6`;
  `:4048-4076 @v7.2.15`, identical text in all three):

```c
	if (likely(skb_frags_readable(skb) ||
		   dev->netmem_tx == NETMEM_TX_NO_DMA))
		goto out;

	if (dev->netmem_tx == NETMEM_TX_NONE)
		goto out_free;

	shinfo = skb_shinfo(skb);

	if (shinfo->nr_frags > 0) {
		niov = netmem_to_net_iov(skb_frag_netmem(&shinfo->frags[0]));
		if (net_is_devmem_iov(niov) &&
		    READ_ONCE(net_devmem_iov_binding(niov)->dev) != dev)
			goto out_free;
	}
```

  So an unreadable skb only reaches a driver that set `netmem_tx`, and only
  if the binding belongs to that netdev.
- `net/core/netdev-genl.c`: `netdev_nl_bind_tx_doit()` returns `-EOPNOTSUPP`
  "Driver does not support netmem TX" (`:1241-1246 @v7.4-rc3`,
  `:1201 @v7.2.6`).

Bind also needs a DMA device: `netdev_queue_get_dma_dev(bind_dev, 0,
NETDEV_QUEUE_TYPE_TX)` (`netdev-genl.c:1259 @v7.4-rc3`), which without
`queue_mgmt_ops->ndo_queue_get_dma_dev` is `dev->dev.parent` if it has a
`dma_mask` (`net/core/netdev_queues.c:10-21 @v7.4-rc3`, same `@v7.2.6`).
ionic sets the parent with `SET_NETDEV_DEV(netdev, dev)`
(`ionic_lif.c:3256 @v7.4-rc3`) and has no `queue_mgmt_ops`. INFERRED: the
parent is the PCI device and equals `q->dev` used for mapping; confirm in the
ionic notes.

### 2.3 7d3ab852dcd8 "net: convert netmem_tx flag to enum" (v7.2-rc1)

Message: "...convert the boolean dev->netmem_tx into an enum:
NETMEM_TX_NONE - no netmem TX support (drop unreadable skbs);
NETMEM_TX_DMA - full support, device does DMA. Update the existing NIC
drivers (bnxt, gve, mlx5, fbnic) and the validators in net/core to use the
new enum. No functional change." Acked-by Harshitha Ramamurthy, Stanislav
Fomichev. Link `20260514-tcp-dm-netkit-v5-1-...@meta.com` (v5 of the netkit
series by Bobby Eshleman).

Hunks: one-line `= true` -> `= NETMEM_TX_DMA` in each of the four drivers;
enum added to `netdevice.h`; the two core checks become
`== NETMEM_TX_NONE`; docs updated.

6ce2bb048055 then adds `NETMEM_TX_NO_DMA` and widens the field to 2 bits
(`include/linux/netdevice.h:1822-1826, 2180 @v7.4-rc3`;
`:1805-1809, 2160 @v7.2.6`; `:1811-1815, 2166 @v7.2.15`). 1abe839b34ae makes
bind walk leased RX queues for `NO_DMA` devices and lets
`validate_xmit_unreadable_skb()` pass them.

All three are in v7.2-rc1, so **v7.2.6 and v7.2.15 already have the enum**;
the backport writes `NETMEM_TX_DMA`, not `true`.

### 2.4 Other core behaviour a TX driver depends on

- Mapping: `__skb_frag_dma_map()` returns the binding's address for a net_iov
  and never calls the DMA API:
  - v7.4-rc3 (`include/linux/skbuff.h:3823-3837`):
    `addr = netmem_dma_addr_decode(netmem_get_dma_addr(frag->netmem)); return addr + offset + frag->offset;`
  - v7.2.6 (`skbuff.h:3826-3837`) and v7.2.15 (`:3814-3825`):
    `return netmem_to_net_iov(frag->netmem)->desc.dma_addr + offset + frag->offset;`
  - The difference is 5c729e6c71f1 (v7.4-rc1, not in v7.2.15), which only
    matters on 32-bit with 64-bit DMA addresses. Same macro signature
    `skb_frag_dma_map(dev, frag, offset, size, dir)` at all three tags.
- `skb_frag_is_net_iov()`, `skb_frag_net_iov()`, `skb_frag_page()` (NULL for
  net_iov), `skb_frag_netmem()`, `skb_frag_address[_safe]()`:
  `skbuff.h:3700-3773 @v7.4-rc3`, `:3703-3776 @v7.2.6`.
- `skb_frags_readable()` = `!skb->unreadable`
  (`skbuff.h:1880 @v7.4-rc3`, `:1891 @v7.2.6`); the bit is set by
  `__skb_fill_netmem_desc()` when a net_iov is attached
  (`skbuff.h:2609-2612 @v7.4-rc3`).
- Linearize/pull/copy all fail for unreadable skbs (`net/core/skbuff.c
  @v7.4-rc3`; the same checks exist `@v7.2.6` at the lines in brackets):
  `__pskb_pull_tail()` returns NULL `:2906` [2886], so `skb_linearize()`
  returns -ENOMEM; `skb_copy_bits()` faults `:3062` [3042]; `skb_copy()`
  `:2189` [2186]; `skb_copy_expand()` `:2531` [2516];
  `skb_checksum()` WARNs `:3570` [3550]; `skb_condense()` skips `:7255`
  [7068]. `skb_checksum_help()` returns -EFAULT
  (`net/core/dev.c:3679 @v7.4-rc3`, `:3582 @v7.2.6`).
- What the stack hands the driver (`net/core/datagram.c:706-747 @v7.4-rc3`,
  `zerocopy_fill_skb_from_devmem()`): headers in the linear area, payload
  only in net_iov frags, **one frag per user iovec entry per PAGE_SIZE niov,
  no coalescing**, up to `MAX_SKB_FRAGS` (17 by default,
  `skbuff.h:350-354`); a frag never exceeds PAGE_SIZE
  (`net_devmem_get_niov_at()`, `net/core/devmem.c:454-464 @v7.4-rc3`). An skb
  is all-readable or all-devmem (`datagram.c:715`,
  `net/ipv4/tcp.c:1257 @v7.4-rc3`).
- `netmem.h` comment added by 72ebc685bca9 (v7.4 cycle,
  `include/net/netmem.h:73-84 @v7.4-rc3`): "New code must not assume net_iov
  implies unreadable memory (check readability via netmem_address() or
  skb_frags_readable() instead)" and (`:136-146`) downcasting with
  `netmem_to_page()`/`netmem_to_net_iov()` in callers "is not allowed unless
  a code path strictly requires a specific backing type". Not present
  `@v7.2.6`.

## 3. Driver commits

### 3.1 gve — c32532670cec (v6.16-rc1)

Message: "Use netmem_dma_*() helpers in gve_tx_dqo.c DQO-RDA paths to enable
netmem TX support in that mode. Declare support for netmem TX in GVE DQO-RDA
mode." Acked-by Harshitha Ramamurthy (gve maintainer). Link
`20250508004830.4100853-8-...`. 2 files, +8/-3.

Hunks:
1. `gve_main.c` `gve_probe()`: `if (!gve_is_gqi(priv) && !gve_is_qpl(priv))
   dev->netmem_tx = true;` just before `register_netdev()`. Gated on queue
   format (the QPL modes copy into pre-registered pages), not on features.
   Now `gve_main.c:3079-3080 @v7.4-rc3`, `:2896-2897 @v7.2.6`.
2. `gve_tx_add_skb_no_copy_dqo()` (map): frag loop
   `dma_unmap_addr_set(pkt, dma[n], addr)` ->
   `netmem_dma_unmap_addr_set(skb_frag_netmem(frag), pkt, dma[n], addr)`.
   The descriptor is filled from the local `addr`. Now
   `gve_tx_dqo.c:733-743 @v7.4-rc3`, `:736 @v7.2.6`.
3. `gve_unmap_packet()` (completion/teardown): `dma_unmap_page()` ->
   `netmem_dma_unmap_page_attrs(..., DMA_TO_DEVICE, 0)` for bufs 1..n; buf 0
   (linear) stays `dma_unmap_single()`. Now `gve_tx_dqo.c:170-187 @v7.4-rc3`,
   `:182 @v7.2.6`.

Not touched: the map error path still calls plain `dma_unmap_page()`
(`gve_tx_dqo.c:747-762 @v7.4-rc3`). INFERRED: harmless only because a
net_iov map cannot fail and an skb never mixes net_iov and page frags, so the
error path is unreachable after a net_iov frag; a reviewer could still ask
for it.

Linearize: `gve_try_tx_skb()` calls `skb_linearize()` when a non-GSO skb
needs more than `GVE_TX_MAX_DATA_DESCS` (10) buffers and drops on failure
(`gve_tx_dqo.c:1017-1025 @v7.4-rc3`, `:999 @v7.2.6`). Unchanged by the
netmem commit, so an unreadable skb with too many frags is dropped.
`gve_features_check_dqo()` (`:976-994`) has no readability check.

### 3.2 mlx5e — 5a842c288cfa (v6.17-rc1) and fix 870bc1aaa0f9 (v6.17-rc1)

5a842c288cfa message: "Declare netmem TX support in netdev. As required, use
the netmem aware dma unmapping APIs for unmapping netmems in tx completion
path." Reviewed-by Tariq Toukan, Mina Almasry. Link
`20250616141441.1243044-13-mbloch@nvidia.com` (last patch of the series that
also added unreadable-netmem RX, db3010bb5a01). 2 files, +4/-1.

Hunks:
1. `en/txrx.h` `mlx5e_tx_dma_unmap()`: `MLX5E_DMA_MAP_PAGE` case
   `dma_unmap_page()` -> `netmem_dma_unmap_page_attrs(..., 0)`.
2. `en_main.c` `mlx5e_build_nic_netdev()`: `netdev->netmem_tx = true;`
   unconditionally (now `en_main.c:5972 @v7.4-rc3` and `@v7.2.6`).

It was **incomplete**: the map side still stored the real address, so the
unmap helper never skipped. 870bc1aaa0f9: "net_iovs should have the dma
address set to 0 so that netmem_dma_unmap_page_attrs() correctly skips the
unmap. This was not done in mlx5 when support for devmem tx was added and
resulted in the splat below when the platform iommu was enabled." (WARNING at
`drivers/iommu/dma-iommu.c:1228 iommu_dma_unmap_page` from
`mlx5e_poll_tx_cq`.) Reported-by Stanislav Fomichev, `Fixes: 5a842c288cfa`,
Acked-by Stanislav Fomichev, Reviewed-by Carolina Jubran.

Hunks: `mlx5e_dma_push()` split into `mlx5e_dma_push_single()` and
`mlx5e_dma_push_netmem(sq, netmem, addr, size)` which does
`netmem_dma_unmap_addr_set(netmem, dma, addr, addr)`
(`en/txrx.h:320-339 @v7.4-rc3`, `:336 @v7.2.6`); callers in
`mlx5e_txwqe_build_dsegs()` (`en_tx.c:199,217 @v7.4-rc3`), the MPWQE path and
kTLS resync dump updated. The data segment address is written from the local
`dma_addr`, so zeroing the stored copy is safe.

Lesson: **test with an IOMMU enabled**; without one the wrong unmap is
silent.

No linearize, counter, or `ndo_features_check` change.

### 3.3 bnxt — 4922ca773d9d (v6.17-rc1)

Message: "Use netmem_dma_*() helpers and declare netmem_tx to support netmem
TX. By this change, all bnxt devices will support the netmem TX. Unreadable
skbs are not going to be handled by the TX push logic. So, it checks whether
a skb is readable or not before the TX push logic. netmem TX can be tested
with ncdevmem.c". Author Taehee Yoo; Acked-by Mina Almasry, Stanislav
Fomichev; Reviewed-by Michael Chan. Link
`20250619144058.147051-1-ap420073@gmail.com` (single patch). 1 file,
+24/-16. Companion core fix 4672aec56d2e (`skb_frag_address_safe()` returns
NULL when the frag has no page; "Cc: ap420073@gmail.com") came out of the
same review.

Hunks (`bnxt.c`):
1. `bnxt_start_xmit()` push path (copies the whole packet into the push
   buffer): condition gains `skb_frags_readable(skb) &&`
   (`:608-609 @v7.4-rc3`, `:579 @v7.2.6`). This is the only "pull/copy path"
   change in any driver.
2. Same function, map loop: `dma_unmap_addr_set(tx_buf, mapping, mapping)` ->
   `netmem_dma_unmap_addr_set(skb_frag_netmem(frag), tx_buf, mapping,
   mapping)` (`:754 @v7.4-rc3`, `:732 @v7.2.6`); descriptor filled from the
   local `mapping`.
3. Same function, `tx_dma_error` unwind: `dma_unmap_page()` ->
   `netmem_dma_unmap_page_attrs()` (`:807-815 @v7.4-rc3`, `:792 @v7.2.6`).
4. `__bnxt_tx_int()` completion: same replacement (`:897 @v7.4-rc3`,
   `:878 @v7.2.6`).
5. `bnxt_free_one_tx_ring_skbs()` teardown: same (`:3539 @v7.4-rc3`,
   `:3498 @v7.2.6`).
6. `bnxt_init_one()`: `dev->netmem_tx = true;` unconditional
   (`:17474 @v7.4-rc3` as `NETMEM_TX_DMA`, `:17262 @v7.2.6`).

bnxt is the model for "replace every unmap site": map error, completion and
ring teardown.

### 3.4 fbnic — b127e355f1af (v6.18-rc1), the smallest reference

Message: "Support devmem Tx. We already use skb_frag_dma_map(), we just need
to make sure we don't try to unmap the frags. Check if frag is unreadable and
mark the ring entry." followed by the selftest output:

```
  # ./tools/testing/selftests/drivers/net/hw/devmem.py
  TAP version 13
  1..3
  ok 1 devmem.check_rx
  ok 2 devmem.check_tx
  ok 3 devmem.check_tx_chunks
  # Totals: pass:3 fail:0 xfail:0 xpass:0 skip:0 error:0
```

Author Jakub Kicinski; Acked-by Mina Almasry. Link
`20250916145401.1464550-1-kuba@kernel.org` (single patch). 2 files, +15/-1.

fbnic keeps no per-buffer DMA address (it re-reads it from the descriptor in
`fbnic_unmap_page_twd()`), so it cannot store 0; it marks the otherwise
unused `ring->tx_buf[]` slot of each frag descriptor instead.

Hunks:
1. `fbnic_netdev.c` `fbnic_netdev_alloc()`: `netdev->netmem_tx = true;`
   (`:762 @v7.4-rc3` and `@v7.2.6` as `NETMEM_TX_DMA`).
2. `fbnic_txrx.c`: `#define FBNIC_XMIT_NOUNMAP ((void *)1)`
   (`:42 @v7.4-rc3`, `:40 @v7.2.6`).
3. `fbnic_tx_map()`: `bool is_net_iov`, false for the head, set from
   `skb_frag_is_net_iov(frag)` before each `skb_frag_dma_map()`; when set,
   `ring->tx_buf[tail] = FBNIC_XMIT_NOUNMAP`.
4. `fbnic_tx_map()` `dma_error` unwind: marked slots are reset to NULL
   instead of unmapped.
5. `fbnic_clean_twq0()` completion: same test, and the slot is reset.

Current code, `drivers/net/ethernet/meta/fbnic/fbnic_txrx.c @v7.4-rc3`:

```c
/* :42 */
#define FBNIC_XMIT_NOUNMAP	((void *)1)

/* :152-174 */
static void fbnic_unmap_single_twd(struct device *dev, __le64 *twd)
{
	u64 raw_twd = le64_to_cpu(*twd);
	unsigned int len;
	dma_addr_t dma;

	dma = FIELD_GET(FBNIC_TWD_ADDR_MASK, raw_twd);
	len = FIELD_GET(FBNIC_TWD_LEN_MASK, raw_twd);

	dma_unmap_single(dev, dma, len, DMA_TO_DEVICE);
}

static void fbnic_unmap_page_twd(struct device *dev, __le64 *twd)
{
	u64 raw_twd = le64_to_cpu(*twd);
	unsigned int len;
	dma_addr_t dma;

	dma = FIELD_GET(FBNIC_TWD_ADDR_MASK, raw_twd);
	len = FIELD_GET(FBNIC_TWD_LEN_MASK, raw_twd);

	dma_unmap_page(dev, dma, len, DMA_TO_DEVICE);
}

/* :339-429 */
static bool
fbnic_tx_map(struct fbnic_ring *ring, struct sk_buff *skb, __le64 *meta)
{
	struct device *dev = skb->dev->dev.parent;
	unsigned int tail = ring->tail, first;
	unsigned int size, data_len;
	skb_frag_t *frag;
	bool is_net_iov;
	dma_addr_t dma;
	__le64 *twd;

	ring->tx_buf[tail] = skb;

	tail++;
	tail &= ring->size_mask;
	first = tail;

	size = skb_headlen(skb);
	data_len = skb->data_len;

	if (size > FIELD_MAX(FBNIC_TWD_LEN_MASK))
		goto dma_error;

	is_net_iov = false;
	dma = dma_map_single(dev, skb->data, size, DMA_TO_DEVICE);

	for (frag = &skb_shinfo(skb)->frags[0];; frag++) {
		twd = &ring->desc[tail];

		if (dma_mapping_error(dev, dma))
			goto dma_error;

		*twd = cpu_to_le64(FIELD_PREP(FBNIC_TWD_ADDR_MASK, dma) |
				   FIELD_PREP(FBNIC_TWD_LEN_MASK, size) |
				   FIELD_PREP(FBNIC_TWD_TYPE_MASK,
					      FBNIC_TWD_TYPE_AL));
		if (is_net_iov)
			ring->tx_buf[tail] = FBNIC_XMIT_NOUNMAP;

		tail++;
		tail &= ring->size_mask;

		if (!data_len)
			break;

		size = skb_frag_size(frag);
		data_len -= size;

		if (size > FIELD_MAX(FBNIC_TWD_LEN_MASK))
			goto dma_error;

		is_net_iov = skb_frag_is_net_iov(frag);
		dma = skb_frag_dma_map(dev, frag, 0, size, DMA_TO_DEVICE);
	}

	*twd |= FBNIC_TWD_TYPE(LAST_AL);

	FBNIC_XMIT_CB(skb)->desc_count = ((twd - meta) + 1) & ring->size_mask;

	ring->tail = tail;

	/* Record SW timestamp */
	skb_tx_timestamp(skb);

	/* Verify there is room for another packet */
	fbnic_maybe_stop_tx(skb->dev, ring, FBNIC_MAX_SKB_DESC);

	if (fbnic_tx_sent_queue(skb, ring))
		fbnic_tx_doorbell(ring, meta);
	else
		ring->deferred_meta = meta - ring->desc;

	return false;
dma_error:
	if (net_ratelimit())
		netdev_err(skb->dev, "TX DMA map failed\n");

	while (tail != first) {
		tail--;
		tail &= ring->size_mask;
		twd = &ring->desc[tail];
		if (tail == first)
			fbnic_unmap_single_twd(dev, twd);
		else if (ring->tx_buf[tail] == FBNIC_XMIT_NOUNMAP)
			ring->tx_buf[tail] = NULL;
		else
			fbnic_unmap_page_twd(dev, twd);
	}

	return true;
}

/* :606-619, inside fbnic_clean_twq0() */
		fbnic_unmap_single_twd(nv->dev, &ring->desc[head]);
		head++;
		head &= ring->size_mask;
		desc_cnt--;

		while (desc_cnt--) {
			if (ring->tx_buf[head] != FBNIC_XMIT_NOUNMAP)
				fbnic_unmap_page_twd(nv->dev,
						     &ring->desc[head]);
			else
				ring->tx_buf[head] = NULL;
			head++;
			head &= ring->size_mask;
		}
```

v7.2.6 has the same netmem logic at `fbnic_txrx.c:40, 315-404, 587`; the only
differences in these functions between v7.2.6 and v7.4-rc3 are the doorbell
refactor (`fbnic_tx_doorbell()`/`fbnic_tx_flush_doorbell()`, `deferred_meta`),
unrelated to netmem (`git diff v7.2.6 v7.4-rc3`).

`fbnic_xmit_frame_ring()` (`:433-479`) has no linearize path (one descriptor
per frag, `nr_frags + 10` descriptors reserved) and `fbnic_features_check()`
(`:542-568`) has no readability test. No counter was added; drops go to the
existing `ring->stats.dropped`.

### 3.5 bnge — bd5ad9c052c8 "bng_en: Add TX support" (v7.0-rc1)

A new driver's TX path written with the helpers from the start
(`bnge_txrx.c:1078, 1575, 1630 @v7.4-rc3` and `@v7.2.6`), but no
`netmem_tx` assignment anywhere in `drivers/net/ethernet/broadcom/bnge`
`@v7.4-rc3`. So it is netmem-safe but core still drops unreadable skbs for
it.

### 3.6 netkit — 6ce2bb048055 (v7.2-rc1)

`dev->netmem_tx = NETMEM_TX_NO_DMA;` (`drivers/net/netkit.c:470`). Not a
model for ionic.

## 4. Comparison

| | per-frag unmap state | unmap sites converted | unreadable + too many frags | gating of netmem_tx | counters | features_check |
|---|---|---|---|---|---|---|
| gve | `netmem_dma_unmap_addr_set()` stores 0 in `pkt->dma[i]` | completion/teardown (`gve_unmap_packet`); map error path left | `skb_linearize()` fails -> drop (pre-existing) | `!gqi && !qpl` (queue format) | none | unchanged |
| mlx5e | `netmem_dma_unmap_addr_set()` stores 0 in `dma->addr`, type `MLX5E_DMA_MAP_PAGE` | single helper `mlx5e_tx_dma_unmap()` | none needed | unconditional | none | unchanged |
| bnxt | `netmem_dma_unmap_addr_set()` stores 0 in `tx_buf->mapping` | map error, completion, ring teardown | none; push (copy) path skipped via `skb_frags_readable()` | unconditional | none | unchanged |
| fbnic | marker `FBNIC_XMIT_NOUNMAP` in `ring->tx_buf[slot]` from `skb_frag_is_net_iov()` | map error, completion | none needed | unconditional | none | unchanged |

`grep` for `skb_frags_readable|skb_frag_is_net_iov|netmem_dma_unmap|->unreadable`
under `drivers/` returns only the lines above at both v7.4-rc3 and v7.2.6.

## 5. What this means for ionic (VERIFIED in ionic source unless noted)

`drivers/net/ethernet/pensando/ionic/ionic_txrx.c @v7.4-rc3`:

- Map: `ionic_tx_map_frag()` already uses `skb_frag_dma_map()` (`:1110`), so
  net_iov frags map correctly today.
- State: `ionic_tx_map_skb()` stores `buf_info->dma_addr`/`len`
  (`:1133-1145`); `struct ionic_buf_info { struct page *page; dma_addr_t
  dma_addr; u32 page_offset; u32 len; }` (`ionic_dev.h:206-211`).
- **The stored address is the one given to the device**:
  `ionic_tx_skb_frags()` `elem->addr = cpu_to_le64(buf_info->dma_addr)`
  (`:1643`) and `ionic_tx_tso()` `frag_addr = buf_info->dma_addr` (`:1515`).
  So the gve/bnxt/mlx5 "store 0" idiom is wrong for ionic as structured; use
  a per-buffer marker (fbnic style). INFERRED options: a flag in
  `ionic_buf_info` (e.g. reuse `page`/`page_offset`, which the skb TX path
  does not use, or add a bool), set from `skb_frag_is_net_iov(frag)`.
- Unmap sites to convert:
  1. `ionic_tx_map_skb()` `dma_fail` unwind, `dma_unmap_page()` (`:1152-1162`).
  2. `ionic_tx_desc_unmap_bufs()` (`:1165-1183`), used by `ionic_tx_clean()`
     (`:1203`) for completion and teardown, **and** by the XDP TX paths
     (`:292`, `:365`), whose `buf_info` entries must stay "unmap".
- Linearize: `ionic_tx_descs_needed()` calls `skb_linearize()` when a non-GSO
  skb has `nr_frags > q->max_sg_elems` (`:1720-1723`) or a TSO segment spans
  more than `max_sg_elems + 1` buffers (`:1754-1757`), and bumps
  `stats->linearize` (`:1772-1778`). `IONIC_TX_MAX_SG_ELEMS` is 8,
  `_V1` is 15 (`ionic_if.h:1017,1023`); `MAX_SKB_FRAGS` is 17. For an
  unreadable skb the linearize fails and `ionic_start_xmit()` drops
  (`:1860-1863`). INFERRED: TCP then retransmits an identically shaped skb,
  so the flow stalls rather than degrades; the series needs an answer (see
  open questions).
- TSO header fixups `skb_cow_head(skb, 0)` (`:1358,1382`) touch only the
  linear header, which is readable.
- ionic has no `ndo_features_check` (no hit in the ionic directory).
- v7.2.6 vs v7.4-rc3: `git diff` of `ionic_txrx.c` is a single hunk at
  `@@ -1672` (doorbell/`netif_txq_maybe_stop()` rework in `ionic_tx()`), so
  everything above line 1672 has the same line numbers at v7.2.6 and
  `ionic_tx_descs_needed()` is 7 lines earlier there. v7.2.15's
  `ionic_txrx.c` differs from v7.2.6 by one 13-line change (INFERRED to be
  the same fix backported to stable; not read).

## 6. Reviewer checklist for "ionic: support netmem TX"

1. No `dma_unmap_page*()` on a net_iov frag address on **any** path: map
   error unwind, TX completion, queue teardown/flush, TSO. (netmem.rst TX
   requirement 1; the mlx5 fix 870bc1aaa0f9 is what happens otherwise.)
2. The address written to the descriptor is still the real one (do not zero
   the only copy).
3. Head (`dma_map_single`) and XDP buffers are still unmapped; marker is
   reset when the slot is reused.
4. Nothing reads frag payload: no copy/inline/push, no `skb_frag_address()`,
   `skb_frag_page()`, `kmap`, no software checksum or padding over frag data.
   If such a path exists, gate it on `skb_frags_readable()` as bnxt did.
5. Behaviour when `skb_linearize()` fails for an unreadable skb is defined
   and counted, not a silent stall. Say what happens in the commit message.
6. Use `skb_frag_is_net_iov()`/`skb_frag_netmem()` and helpers; do not
   downcast (`netmem_to_net_iov()`) or assume net_iov means unreadable
   (`netmem.h:73-84,136-146 @v7.4-rc3`).
7. `netdev->netmem_tx = NETMEM_TX_DMA` set before `register_netdev()`, only
   for configurations whose TX path satisfies 1-5 (gve precedent for
   conditional enabling). It is a device flag, not a feature bit: no
   `NETIF_F_*`, no ethtool knob.
8. The DMA device used for mapping equals what
   `netdev_queue_get_dma_dev()` returns (the netdev parent), or the dma-buf is
   attached to the wrong device.
9. Tested with `tools/testing/selftests/drivers/net/hw/devmem.py`
   (`check_tx`, `check_tx_chunks`), output quoted in the commit message
   (fbnic precedent), with an IOMMU enabled. Both tests send two 6-byte lines;
   `check_tx_chunks` uses 3-byte iovecs (`devmem_lib.py:257-269`,
   `ncdevmem.c:1358-1373 @v7.4-rc3`), i.e. 2 frags per skb, so they do not
   exercise frag overflow.
10. Patch is minimal, no unrelated refactoring; prep refactors are separate
    "no functional change" patches. Target `net-next`. Cc Mina Almasry and
    Stanislav Fomichev (acked every driver patch so far) and the ionic
    maintainers.

## 7. Conventional series shape and subjects

Precedent: fbnic and bnxt were a **single patch**; gve was one driver patch
inside the core series; mlx5 was the last patch of its netmem series.
Subject styles used: "eth: fbnic: support devmem Tx", "eth: bnxt: add netmem
TX support", "gve: add netmem TX support to GVE DQO-RDA mode",
"net/mlx5e: Add TX support for netmems". ionic's own prefix is `ionic:`.

Suggested (INFERRED from the above, not an upstream series):

```
[PATCH net-next 0/3] ionic: support netmem TX
[PATCH net-next 1/3] ionic: record whether a Tx buffer needs a DMA unmap
[PATCH net-next 2/3] ionic: don't linearize unreadable skbs
[PATCH net-next 3/3] ionic: add netmem TX support
```

1/3 is a no-functional-change prep (marker in `ionic_buf_info`, unmap sites
honour it); 2/3 defines the frag-overflow behaviour; 3/3 sets the marker from
`skb_frag_is_net_iov()` and sets `netmem_tx = NETMEM_TX_DMA`, with the
devmem.py output. If 2/3 turns out to be only a counter, fold it into 3/3 and
send two patches, or one if the marker is trivial.

## 8. Backport: v7.2.6 sources on a v7.2.15 kernel

How the v7.2.6-era drivers do it: identically to v7.4-rc3. Every netmem TX
line quoted in section 3 exists at v7.2.6 with the line numbers given there,
and `git diff v7.2.6 v7.4-rc3` of the fbnic TX functions shows no
netmem-related change.

| helper / symbol | v7.2.6 | v7.2.15 | v7.4-rc3 | difference |
|---|---|---|---|---|
| `enum netmem_tx_mode`, `NETMEM_TX_DMA` | `netdevice.h:1805-1809` | `:1811-1815` | `:1822-1826` | none |
| `net_device.netmem_tx:2` | `:2160` | `:2166` | `:2180` | none |
| `netmem_dma_unmap_addr_set()` | `netmem.h:397` | `:397` | `:447` | none |
| `netmem_dma_unmap_page_attrs()` | `netmem.h:405` | `:405` | `:455` | none |
| `skb_frag_dma_map()` / `__skb_frag_dma_map()` | `skbuff.h:3826-3858` | `:3814-3846` | `:3823-3858` | net_iov branch uses `netmem_dma_addr_decode()` only in v7.4 (5c729e6c71f1); same result on 64-bit |
| `skb_frag_is_net_iov()` | `skbuff.h:3703` | present | `:3700` | none |
| `skb_frag_netmem()` | `skbuff.h:3744` | present | `:3741` | none |
| `skb_frags_readable()` | `skbuff.h:1891` | present | `:1880` | none |
| `validate_xmit_unreadable_skb()` | `dev.c:3991` | `:4048` | `:4169` | none |
| `netdev_nl_bind_tx_doit()` netmem_tx check | `netdev-genl.c:1201` | `:1201` | `:1241` | none |
| `netdev_queue_get_dma_dev()` | `netdev_queues.c:36` | not re-read | `:36` | none between v7.2.6 and v7.4-rc3 |
| `get_netmem()`/`put_netmem()` | present | present | renamed `netmem_inc_ref()`/`netmem_dec_ref()` (2620efb2cec1) | core only; a TX driver does not call them |
| `net_iov_area.base_virtual` | present | present | removed (7d3e5984d0fc) | core only |
| netmem.h "don't assume net_iov is unreadable" comment | absent | absent | present | guidance only |

`git diff v7.2.6 v7.2.15` over `netmem.h`, `skbuff.h`, `dev.c`, `skbuff.c`,
`netdevice.h` shows no change to any netmem/unreadable line except brace
style in one `skb_frags_readable()` test; `netmem.h` is unchanged.

Selftests `devmem.py`, `devmem_lib.py`, `ncdevmem.c` exist at v7.2.6.

Conclusion: the mainline ionic patch should apply to the v7.2.6 ionic sources
with at most line-offset fuzz in `ionic_txrx.c` below line 1672, and build
against v7.2.15 headers unchanged.

## 9. Open questions

1. Frag overflow: with FW/emulator `max_sg_elems` of 8 or 15 and up to 17
   net_iov frags, what should ionic do? Options: drop and count (upstream
   precedent, gve), or split the skb across descriptors. Which SG version does
   rocm-ernic negotiate (see `ernic-*.md`)?
2. Does any ionic TX path other than those read touch frag payload
   (hwstamp queue path `ionic_start_hwstamp_xmit()`, padding)? Not fully read
   here.
3. Is `netdev->dev.parent` the same `struct device` as `q->dev` for every
   ionic bus type (PCI PF/VF)? Needed for checklist item 8.
4. Review threads were not fetched (no network use); themes above come only
   from trailers, commit text and follow-up fixes.
5. The v7.2.15 kernel lacks 5c729e6c71f1; irrelevant on x86-64, but note it
   if the DKMS module is ever built for a 32-bit LPAE target.

## Verification pass

Independent re-check, 2026-11-18, against `/var/tmp/ionic-devmem/linux`
(`git show <tag>:path`, `git grep <tag>`, `git diff`). Paths under
`drivers/net/ethernet/pensando/ionic/` are abbreviated. Nothing above this
heading was edited.

### Claims

1. Only bnxt, gve, mlx5e, fbnic set `NETMEM_TX_DMA`, netkit `NO_DMA`; ice,
   idpf, tg3, virtio_net do not; bnge uses helpers only. **CONFIRMED**
   (`git grep -E 'netmem_tx|NETMEM_TX' -- drivers` at v7.4-rc3, v7.2.6,
   v7.2.15: `bnxt.c:17474`, `gve_main.c:3080`, `en_main.c:5972`,
   `fbnic_netdev.c:762`, `netkit.c:470 @v7.4-rc3`; nothing else).
2. Commit inventory (sha, date, subject, first release, diffstat).
   **CONFIRMED** for all 14 rows with `git log -1` and
   `git describe --contains`; 7d3ab852dcd8, 6ce2bb048055, 1abe839b34ae are
   ancestors of v7.2.6 and v7.2.15; 5c729e6c71f1 is in neither.
   `git log -S'netmem_tx'` finds no driver commit beyond those listed.
3. Helper text and "store 0 / skip 0" semantics. **CONFIRMED**
   (`include/net/netmem.h:447-464 @v7.4-rc3`, `:397-414 @v7.2.6` and
   `@v7.2.15`, identical).
4. `validate_xmit_unreadable_skb()` and bind-tx `-EOPNOTSUPP`. **CONFIRMED**
   (`net/core/dev.c:4169-4197`, `net/core/netdev-genl.c:1241-1246 @v7.4-rc3`;
   `netdev-genl.c:1204,1219 @v7.2.6` and `@v7.2.15`). Line `:1201` cited
   above is the `if`, the message is `:1204`.
5. Mapping needs no change; `__skb_frag_dma_map()` difference is only
   `netmem_dma_addr_decode()`. **CONFIRMED** (`include/linux/skbuff.h:3823-3837
   @v7.4-rc3`, `:3814-3825 @v7.2.15`; decode is identity unless
   `NETMEM_32BIT_ARCH_WITH_64BIT_DMA`, `netmem.h:379-385 @v7.4-rc3`).
6. fbnic commit, hunks and quoted current code. **CONFIRMED** against
   `git show b127e355f1af` and `fbnic_txrx.c:42,376,390,422-423,612-616
   @v7.4-rc3`.
7. gve, mlx5e hunks, mlx5 fix and IOMMU splat. **CONFIRMED**
   (`gve_tx_dqo.c:733-762,1017-1025`, `gve_main.c:3079-3080`,
   `en/txrx.h:320-339,364-376`, `en_tx.c:199,217`, `ktls_tx.c:747 @v7.4-rc3`;
   870bc1aaa0f9 body).
8. bnxt: "unreadable + too many frags: none". **WRONG (incomplete).** bnxt has
   a pre-existing linearize-or-drop path, untouched by 4922ca773d9d:
   `bnxt.c:509-516 @v7.4-rc3` (`:505 @v7.2.6`),
   `#if (MAX_SKB_FRAGS > TX_MAX_FRAGS) ... if (skb_linearize(skb)) goto
   tx_free;`. So two of four reference drivers (gve, bnxt) drop an unreadable
   skb with too many frags, and neither netmem commit addressed it. The rest of
   the bnxt section (push gate `:608-609`, map `:754`, unwind `:811`,
   completion `:897`, teardown `:3539`, flag `:17474`) is CONFIRMED.
9. No driver adds counters, touches `ndo_features_check`, or gates on netdev
   features. **CONFIRMED** for drivers (`git grep -E
   'skb_frags_readable|skb_frag_is_net_iov|->unreadable' v7.4-rc3 -- drivers`
   returns only `bnxt.c:609` and `fbnic_txrx.c:390`). **But the notes omit
   that core gates on features**, which matters for ionic:
   - `tcp_sendmsg_locked()` fails devmem sends with `-EINVAL` unless
     `sk->sk_route_caps & NETIF_F_SG` (`net/ipv4/tcp.c:1159-1162,1178-1181
     @v7.4-rc3`).
   - `skb_checksum_help()` returns `-EFAULT` for unreadable skbs
     (`net/core/dev.c:3679 @v7.4-rc3`), so without `NETIF_F_HW_CSUM` every
     devmem TCP segment is dropped in `validate_xmit_skb()`.
   - ionic sets `NETIF_F_SG` only if the device grants `IONIC_ETH_HW_TX_SG`
     and `NETIF_F_HW_CSUM` only with `IONIC_ETH_HW_TX_CSUM`
     (`ionic_lif.c:1615-1619 @v7.4-rc3`). The emulator notes
     (`ernic-vm-stack.md:14-17,292-301`) say rocm-ernic grants neither today.
   So devmem TX on ionic is a driver-only change **for hardware that offers
   TX_SG and TX_CSUM**, not for rocm-ernic as it stands.
10. ionic cannot use the "store 0" idiom on `buf_info->dma_addr`.
    **CONFIRMED** (`ionic_txrx.c:1515` TSO, `:1643` SG elems, also `:1577`
    and `:1616` read `bufs[0].dma_addr` for the head `@v7.4-rc3`; same lines
    `@v7.2.6`).
11. "INFERRED option: reuse `page`/`page_offset`, which the skb TX path does
    not use". **WRONG as stated (unsafe).** XDP TX shares the same
    `q->tx_info[]` slots and writes `bi->page` for head and frags
    (`ionic_txrx.c:331-332,371 @v7.4-rc3`); nothing clears `bufs[]` on
    completion (`ionic_tx_desc_unmap_bufs()` only zeroes `nbufs`, `:1182`). A
    marker reusing `page` would read stale XDP values. Any marker must be
    written, true or false, for every buffer on every map path:
    `ionic_tx_map_skb()` head and frags (`:1133-1146`) and
    `ionic_xdp_post_frame()` (`:330-333,369-371`). A dedicated field is
    simpler; `struct ionic_buf_info` has no padding to reuse
    (`ionic_dev.h:206-211`), so it grows `bufs[MAX_SKB_FRAGS + 1]`
    (`:222`) unless `len` or `page_offset` is narrowed.
12. ionic unmap sites are exactly the `dma_fail` unwind and
    `ionic_tx_desc_unmap_bufs()`. **CONFIRMED** (only `dma_unmap_page()`
    calls in the file: `ionic_txrx.c:1157,1179 @v7.4-rc3`; callers of the
    latter `:292` XDP_REDIRECT clean, `:365` XDP map error, `:1203` skb
    clean). XDP_TX buffers are page-pool mapped and never unmapped here.
13. "`max_sg_elems` is 8 or 15". **WRONG.** `ionic_lif_queue_identify()`
    overwrites the device's answer:
    `qti->max_sg_elems = min_t(u16, max_frags - 1, MAX_SKB_FRAGS);` with
    `max_frags = IONIC_TX_MAX_FRAGS = 16` for a TXQ
    (`ionic_lif.c:3982-3989 @v7.4-rc3`, `:3986 @v7.2.6`, copied at `:616`).
    So `q->max_sg_elems` is always **15**, whatever SG version is negotiated
    (already recorded in `ionic-tx-device-contract.md:257-258`). Consequences:
    - Non-GSO overflow needs `nr_frags` of 16 or 17 (default
      `MAX_SKB_FRAGS` 17, range 17-45, `net/Kconfig:292-295`); TSO overflow
      needs one segment spanning more than 16 buffers
      (`ionic_txrx.c:1720,1754`). Both need user iovecs of roughly 90 bytes
      or less; page-sized iovecs never hit it.
    - Open question 1 ("which SG version does rocm-ernic negotiate") does not
      change the linearize threshold. It changes whether a 9-15 frag skb
      overruns an 8-element v0 SG slot, which is an emulator-side concern.
14. Linearize fails for unreadable skbs and ionic drops. **CONFIRMED**
    (`net/core/skbuff.c:2906-2907 @v7.4-rc3`; `ionic_txrx.c:1772-1778`,
    `:1859-1861`, `:1874-1877`). The hwstamp path uses the same
    `ionic_tx_descs_needed()`/`ionic_tx()`/`ionic_tx_tso()` (`:1795-1811`),
    which answers open question 2: no skb TX path reads frag payload (the only
    `skb_frag_page()` uses are XDP, `:354,371`).
15. TSO `skb_cow_head(skb, 0)` is safe. **CONFIRMED**, including the
    header-cloned case: `pskb_expand_head()` copies only the linear part
    (`skbuff.c:2306-2345 @v7.4-rc3`) and `skb_orphan_frags()` returns early
    for MSG_ZEROCOPY uargs because of `SKBFL_DONT_ORPHAN`
    (`skbuff.h:3430-3437`, `skbuff.c:1750`).
16. One frag per iovec, no coalescing, frag at most PAGE_SIZE, limit
    `MAX_SKB_FRAGS`. **CONFIRMED** (`net/core/datagram.c:706-747`,
    `net/core/devmem.c:454-464 @v7.4-rc3`). TCP also refuses to collapse
    unreadable skbs on retransmit (`net/ipv4/tcp_output.c:3561`).
17. Selftests produce about 2 frags. **CONFIRMED**
    (`devmem_lib.py:257-269`, `ncdevmem.c:1358-1373 @v7.4-rc3`: "hello\nworld"
    with `chunk_size=3`). Addition: the same `ncdevmem` chunk option with one
    input line of 48 bytes or more gives 16+ frags in one skb and reproduces
    the overflow by hand.
18. Open question 3 (netdev parent equals `q->dev`). **RESOLVED, same
    device**: `ionic->dev = &pdev->dev` (`ionic_bus_pci.c:320`, the only bus
    file), `SET_NETDEV_DEV(netdev, ionic->dev)` (`ionic_lif.c:3237,3256`),
    `new->q.dev = lif->ionic->dev` (`ionic_lif.c:567,580`), DMA mask set at
    `ionic_bus_pci.c:325 @v7.4-rc3`. ionic has no `queue_mgmt_ops`, so
    `netdev_queue_get_dma_dev()` returns the parent
    (`net/core/netdev_queues.c:9-21`, identical `@v7.2.6`, `@v7.2.15`).
19. Backport table: helpers source-identical across the three tags.
    **CONFIRMED** (`git diff --stat v7.2.6 v7.2.15` over `netmem.h`,
    `devmem.c`, `datagram.c`, `netdev-genl.c`, `netdev_queues.c` is empty;
    `skbuff.h` changes do not touch the netmem helpers).
20. ionic `ionic_txrx.c` v7.2.6 vs v7.4-rc3 is one hunk at 1672.
    **CONFIRMED**. Open question 5 is **RESOLVED**: v7.2.15's
    `ionic_txrx.c` is byte-identical to v7.4-rc3
    (`git diff --quiet v7.2.15 v7.4-rc3`), the difference from v7.2.6 being
    the stable backport c91b4d6e5cc3 of e247d033dd32 "ionic: use
    netif_txq_maybe_stop() in ionic_tx()". No other ionic file changed between
    v7.2.6 and v7.2.15.
21. Series convention and suggested subjects. Precedent (single patches for
    fbnic and bnxt) **CONFIRMED** from Link tags; the three-patch split is the
    author's suggestion and stays INFERRED. Given item 13, "don't linearize
    unreadable skbs" is a corner case, not a blocker.
22. Review themes. **UNVERIFIABLE** beyond trailers; lore was not fetched in
    either pass.

### Not checked

Line numbers of secondary citations in section 2.4 (`skb_copy_bits`,
`skb_checksum`, `skb_condense`) at v7.2.6, and the `en_main.c`/`fbnic` line
numbers at v7.2.6 other than the `netmem_tx` assignments.
