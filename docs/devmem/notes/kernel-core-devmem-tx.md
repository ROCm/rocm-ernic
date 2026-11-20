# Networking-core requirements for devmem TCP TX (v7.4-rc3 / v7.2.15 / v7.2.6)

Read-only research. Trees: `/var/tmp/ionic-devmem/linux` (v7.4-rc3),
`linux-7.2.15`, `linux-7.2.6`. Paths below are relative to the tree root.
Citation form: `file:LINE@7.4` / `@7.2.15` / `@7.2.6`. Where one line number
is followed by "(all)" the function body is byte-identical in the three trees
(compared with md5 of the function text) and only the line numbers shift; the
three line numbers are then given as `7.4 / 7.2.15 / 7.2.6`.

Legend: **[V]** = VERIFIED by reading the source at that tag.
**[I]** = INFERRED (reasoned from verified code, not run).

## 0. Version delta at a glance

| Area | v7.4-rc3 | v7.2.15 | v7.2.6 |
|---|---|---|---|
| `netmem_tx` type | `enum netmem_tx_mode`, 2-bit field | same | same |
| `netdev_nl_bind_tx_doit` | + `PAGE_SHIFT` niov_shift and `nl_sk` args, `WARN_ON_ONCE(nla_put_u32)`; logic identical | = 7.2.6 (file identical) | baseline |
| DMA device | `netdev_queue_get_dma_dev(bind_dev, 0, TX)` -> `ndo_queue_get_dma_dev` or `dev->dev.parent`, must have `dma_mask` | identical | identical |
| dma-buf attach | `dma_buf_dynamic_attach(allow_peer2peer)` + `dma_buf_pin` + `dma_resv_wait_timeout` | static `dma_buf_attach` + `dma_buf_map_attachment_unlocked` | = 7.2.15 (devmem.c/.h identical) |
| Alignment checks | dmabuf size, each sg **dma_addr** and len aligned to PAGE_SIZE; > U32_MAX niovs -> -E2BIG; unrepresentable dma addr -> -EFAULT | TX: dmabuf size and each sg **len** only (sg dma_addr not checked; set_dma_addr result ignored) | = 7.2.15 |
| niov storage | one `net_iov_area` embedded in binding | one `dmabuf_genpool_chunk_owner` + gen_pool chunk per sg entry | = 7.2.15 |
| `net_devmem_get_binding` | also requires same netns and same uid as the netlink socket that bound | no netns/uid check | = 7.2.15 |
| `tcp_sendmsg_locked` dmabuf gate | `sockc.dmabuf_id && (zc != MSG_ZEROCOPY \|\| !binding)` -> `sockc_err ?: -EINVAL`; fastopen/repair + binding -> -EINVAL | `!sockc_err && sockc.dmabuf_id && (...)` -> -EINVAL; no fastopen/repair guard | = 7.2.15 (function identical) |
| `sk_setup_caps` | identical | identical | identical |
| `validate_xmit_unreadable_skb` | identical | identical | identical |
| `skb_copy_and_csum_bits` on unreadable | zero-fills dest, returns 0 | zero-fills dest, returns 0 (stable backport 39d1a6b60183) | returns 0, dest left **uninitialised** |
| `__skb_frag_dma_map` for net_iov | `netmem_dma_addr_decode(netmem_get_dma_addr())+offset+frag->offset` | `niov->desc.dma_addr + offset + frag->offset` | = 7.2.15 |
| ref helpers | `netmem_inc_ref/dec_ref`, `net_iov_inc_ref/dec_ref` | `get_netmem/put_netmem`, `__get_netmem/__put_netmem` | = 7.2.15 |
| netmem.rst "Driver TX Requirements" | identical text (plus new "Design Principles" section above it) | = 7.2.6 | baseline |
| devmem.rst | identical | identical | identical |

For everything devmem-TX relevant, **v7.2.15 == v7.2.6 except** the
`skb_copy_and_csum_bits()` zero-fill (and an RX-only
`skb_frag_is_net_iov` check in `tcp.c:1911@7.2.15`). `net/core/devmem.c`,
`devmem.h`, `netdev-genl.c`, `datagram.c`, `include/net/netmem.h`,
`netdev_queues.c`, `netmem.rst`, `devmem.rst` are file-identical between
7.2.6 and 7.2.15 (`diff -q`). [V]

## 1. `netdev->netmem_tx` and `netdev_nl_bind_tx_doit`

### Type [V]

All three tags: enum, not bool. The bool -> enum conversion is commit
7d3ab852dcd8 "net: convert netmem_tx flag to enum" (first in v7.2-rc1); the
original bool is 383faec0fd64 (v6.16-rc1).

`include/linux/netdevice.h:1822@7.4 / 1811@7.2.15 / 1805@7.2.6`:

```c
enum netmem_tx_mode {
	NETMEM_TX_NONE,		/* no netmem TX support */
	NETMEM_TX_DMA,		/* DMA-capable netmem TX (real HW) */
	NETMEM_TX_NO_DMA,	/* no DMA, e.g. passthrough for virtual devs */
};
```

Field: `unsigned long netmem_tx:2;` inside `struct_group(priv_flags_fast)`
(`netdevice.h:2180@7.4 / 2166@7.2.15 / 2160@7.2.6`). A driver writes
`netdev->netmem_tx = NETMEM_TX_DMA;` (bnxt, mlx5, gve, fbnic do; netkit uses
`NETMEM_TX_NO_DMA`) in all three trees. A backport to 7.2.x therefore needs
**no** bool/enum compat.

- `NETMEM_TX_DMA`: the device itself is the bind target; the core DMA-maps
  the dmabuf for *its* DMA device and only lets unreadable skbs through if
  the first frag's binding was made for exactly this netdev.
- `NETMEM_TX_NO_DMA`: virtual/passthrough device (netkit). Bind walks leased
  RX queues to find a physical `NETMEM_TX_DMA` device and binds there, with
  the virtual dev recorded as `binding->vdev`; `validate_xmit_unreadable_skb`
  passes every unreadable skb without checking the binding.

### Bind flow (`net/core/netdev-genl.c:1195@7.4 / 1155@7.2.15 / 1155@7.2.6`) [V]

Logic identical in all three (7.4 only adds two args and a WARN):

1. Requires `NETDEV_A_DEV_IFINDEX` and `NETDEV_A_DMABUF_FD`, else `-EINVAL`.
2. Unprivileged by design (no GENL_ADMIN_PERM; comment at `1189@7.4`).
3. `netdev_get_by_index_lock()` -> `-ENODEV` if missing or
   `!netif_device_present()`.
4. `netdev->netmem_tx == NETMEM_TX_NONE` -> `-EOPNOTSUPP`, extack
   "Driver does not support netmem TX" (`1241@7.4 / 1201@7.2.x`).
5. `netdev_find_netmem_tx_dev()` (`1163@7.4 / 1129@7.2.x`): returns `dev` for
   `NETMEM_TX_DMA`; else `-EOPNOTSUPP` "No DMA-capable device found for
   netmem TX".
6. `dma_dev = netdev_queue_get_dma_dev(bind_dev, 0, NETDEV_QUEUE_TYPE_TX);`
   (`1259@7.4 / 1219@7.2.x`) -- **queue index 0 only**.
7. `net_devmem_bind_dmabuf(bind_dev, vdev-or-NULL, dma_dev, DMA_TO_DEVICE,
   dmabuf_fd, [PAGE_SHIFT,] priv, [nl_sk,] extack)`.
8. Replies `NETDEV_A_DMABUF_ID = binding->id`. IDs come from an
   `XA_FLAGS_ALLOC1` xarray (`devmem.c:27@7.4`), so the id is never 0 (0 in
   `sockc.dmabuf_id` means "no devmem").

The netdev instance lock is held across the bind (`netdev_get_by_index_lock`
... `netdev_unlock`); **no netdev reference is retained by the binding**
(`binding->dev = dev;` raw pointer, `devmem.c:219@7.4 / 214@7.2.x`). [V]

### Which device is the DMA device [V]

`net/core/netdev_queues.c:10-21` (identical in all three):

```c
	if (queue_ops && queue_ops->ndo_queue_get_dma_dev)
		dma_dev = queue_ops->ndo_queue_get_dma_dev(dev, idx);
	else
		dma_dev = dev->dev.parent;

	return dma_dev && dma_dev->dma_mask ? dma_dev : NULL;
```

So: `dev->queue_mgmt_ops->ndo_queue_get_dma_dev` if the driver provides it,
otherwise `netdev->dev.parent` (what `SET_NETDEV_DEV()` set), and it must
have a non-NULL `dma_mask`. NULL -> `net_devmem_bind_dmabuf` returns
`-EOPNOTSUPP` "Device doesn't support DMA" (`devmem.c:203@7.4 / 198@7.2.x`).
`netdev_queue_get_dma_dev()` asserts the netdev instance lock.

Driver consequence: the `struct device *` the driver passes to its own
`dma_map_*` calls must be the same device (`ionic` uses
`SET_NETDEV_DEV(netdev, dev)` at `ionic_lif.c:3256@7.4`), or it must
implement `ndo_queue_get_dma_dev`. [V for the core rule; I for ionic match]

## 2. `net_devmem_bind_dmabuf`

`net/core/devmem.c:184-372@7.4`, `183-350@7.2.15 == @7.2.6`.

### Attach / map [V]

| | v7.4-rc3 | v7.2.15 / v7.2.6 |
|---|---|---|
| attach | `dma_buf_dynamic_attach(dmabuf, dma_dev, &net_devmem_dmabuf_attach_ops, binding)` with `.allow_peer2peer = true` and no `move_notify`/invalidate (`:54-56`, `:237-239`) | `dma_buf_attach(binding->dmabuf, dma_dev)` (`:229`) |
| pin | `dma_resv_lock(); dma_buf_pin(attachment)` (`:246-251`), "Failed to pin dmabuf" | none (static attachment: exporter pins on map) |
| map | `dma_buf_map_attachment(attachment, direction)` under resv lock (`:253`) | `dma_buf_map_attachment_unlocked(attachment, direction)` (`:236`) |
| fences | `dma_resv_wait_timeout(resv, DMA_RESV_USAGE_KERNEL, true, MAX_SCHEDULE_TIMEOUT)` (`:261`) | none |
| free | resv lock; `dma_buf_unmap_attachment`; `dma_buf_unpin`; unlock; `dma_buf_detach`; `dma_buf_put` (`:77-83`) | `dma_buf_unmap_attachment_unlocked`; `dma_buf_detach`; `dma_buf_put` (`:77-80`) |

7.4 change is commit 69ab87acc614 (v7.4-rc1). Its message states the
practical difference: with the 7.2.x static attach, an exporter must keep the
buffer reachable without P2P ("amdgpu does so by placing it in GTT (system
memory), so a device memory binding silently ends up in host memory"); 7.4
lets a P2P-capable exporter keep it in device memory. `dma_buf_pin()` is a
no-op returning 0 when the exporter has no `.pin` op
(`drivers/dma-buf/dma-buf.c:1134-1147@7.4`), so udmabuf still works. [V]

DMA direction: TX passes `DMA_TO_DEVICE` (bind_tx_doit) and it is stored in
`binding->direction` and used for map and unmap. [V]

### Alignment rules [V]

v7.4-rc3 (`devmem.c:268-320`), with `niov_shift = PAGE_SHIFT` for TX:

```c
	if (!IS_ALIGNED(dmabuf->size, niov_size)) {            /* -EINVAL */
	if ((dmabuf->size >> niov_shift) > U32_MAX) {          /* -E2BIG */
	...
		if (!IS_ALIGNED(dma_addr, niov_size) ||
		    !IS_ALIGNED(len, niov_size)) {              /* -EINVAL */
	...
			if (net_mp_niov_set_dma_addr(niov, dma_addr)) { /* -EFAULT */
```

v7.2.15 / v7.2.6 (`devmem.c:244-281`):

```c
	if (direction == DMA_TO_DEVICE) {
		if (!IS_ALIGNED(dmabuf->size, PAGE_SIZE)) {     /* -EINVAL "TX dma-buf size must be a multiple of PAGE_SIZE" */
	...
		if (!IS_ALIGNED(len, PAGE_SIZE)) {              /* -EINVAL "dma-buf SG length must be PAGE_SIZE aligned" */
```

7.2.x does **not** check that `sg_dma_address()` is page aligned and ignores
the return of `page_pool_set_dma_addr_netmem()` (`:313`). On 64-bit this is
harmless (address stored verbatim, `net/core/page_pool_priv.h:19-33@7.2.6`).

### How net_iov / dma_addr are built [V]

- One `struct net_iov` per PAGE_SIZE of every DMA sg entry
  (`for_each_sgtable_dma_sg`), `net_iov_init(niov, area, NET_IOV_DMABUF)`.
- niov DMA address = `sg_dma_address(sg) + i * PAGE_SIZE`, stored in
  `niov->desc.dma_addr` (7.4: `net_mp_niov_set_dma_addr`, `:326`; 7.2.x:
  `page_pool_set_dma_addr_netmem(..., owner->base_dma_addr + (idx <<
  PAGE_SHIFT))`, `:313` + `:40-48`).
- TX only: `binding->tx_vec[]` (kvmalloc, `dmabuf->size / PAGE_SIZE`
  pointers) maps "dmabuf byte offset / PAGE_SIZE" -> niov
  (`:333@7.4`; `:316@7.2.x` using `owner->area.base_virtual / PAGE_SIZE + i`).
  RX bindings have `tx_vec == NULL`, which is how
  `net_devmem_get_binding()` rejects an RX binding for TX (`-EINVAL`).
- The mapping is created **once at bind time** for `dma_dev`. The driver
  never maps or unmaps a net_iov; it only reads the precomputed address.

### Lifetime, unbind and teardown ordering [V unless marked]

- `binding->ref` is a `percpu_ref` (`:226@7.4 / :218@7.2.x`). Holders: the
  bind itself (initial ref), each in-progress `sendmsg`
  (`net_devmem_lookup_dmabuf` -> `percpu_ref_tryget`), and **one ref per skb
  frag** (`netmem_inc_ref`/`get_netmem` in `zerocopy_fill_skb_from_devmem`,
  plus one per extra frag reference from `skb_frag_ref` in clones' expand,
  `skb_split`, `skb_segment`, ...).
- Unbind happens **only** when the netlink socket that bound is closed:
  `netdev_nl_sock_priv_destroy()` (`netdev-genl.c:1470@7.4 / 1425@7.2.x`) ->
  `net_devmem_unbind_dmabuf()` (`devmem.c:122@7.4 / 124@7.2.x`):
  `xa_erase(id)`; `synchronize_net()`; `list_del`; close bound RX queues
  (none for TX); `percpu_ref_kill(&binding->ref)`. There is no explicit
  unbind netlink op.
- After unbind, new `sendmsg`s fail (`xa_load` misses -> `-EINVAL`).
  In-flight skbs (TCP write/rtx queue, qdisc, driver TX ring) keep their frag
  refs, so the DMA mapping stays valid. When the last ref drops,
  `net_devmem_dmabuf_binding_release` schedules
  `__net_devmem_dmabuf_binding_free` on a workqueue, which unmaps, (7.4:
  unpins,) detaches and `dma_buf_put`s (`:58-89@7.4 / :52-85@7.2.x`).
  The header comment says the same: "net_iovs allocated from this binding
  that are stuck in the TX path for any reason (such as awaiting
  retransmits) hold a reference to the binding until the skb holding them is
  freed" (`devmem.h:46-52@7.4`).
- So the dma-buf mapping outlives unbind until every skb is freed: an
  unACKed TCP segment pins the binding indefinitely. A driver that leaks a TX
  skb leaks the binding and the dmabuf. [I]

### netdev unregister / driver unload with a live TX binding

- [V] `binding->dev` is cleared (`WRITE_ONCE(binding->dev, NULL)`) only in
  `mp_dmabuf_devmem_uninstall()` (`devmem.c:545-564@7.4 / 517-536@7.2.x`),
  which is reached from `dev_memory_provider_uninstall()`
  (`dev.c:12642@7.4 / 12421@7.2.15 / 12333@7.2.6`) iterating **RX queues**,
  and only when `binding->bound_rxqs` becomes empty after erasing a matching
  rxq. A TX binding has no bound rxqs, so nothing in the unregister path
  touches it. `grep` for `binding->dev` in `net/core` shows no other writer.
- [V] The binding holds no `netdev_hold()` and no `get_device()` on
  `dma_dev`.
- [I] Therefore, if the netdev is unregistered/freed (rmmod of the driver,
  PCI unbind) while the userspace netlink socket still holds a TX binding:
  - `binding->dev` is a stale pointer. `netdev_nl_sock_priv_destroy()` later
    does `dev = binding->dev; ... netdev_hold(dev, ...); netdev_lock(dev);`
    on it -- use-after-free at netlink socket close.
  - `validate_xmit_unreadable_skb()` and `net_devmem_get_binding()` only
    compare the pointer, so they just stop matching (drop / `-ENODEV`).
  - The dma-buf attachment/mapping against the PCI device persists past
    driver unbind and is torn down later against a device with no driver.
  - Skbs still in the driver TX ring at `ndo_stop` are freed by the driver's
    normal ring cleanup; that drops frag refs normally. The driver must not
    `dma_unmap` their net_iov frags (section 6).
  Test-procedure rule: close the bind netlink socket (and let sockets drain)
  **before** unloading the DKMS module. This looks like an upstream gap in
  all three tags, not something a driver can fix.

## 3. sendmsg path

### Socket and message requirements [V]

- `SO_ZEROCOPY` (`net/core/sock.c:1605-1620@7.4`; sets `SOCK_ZEROCOPY`; TCP
  and UDP inet sockets only). Without it `uarg` stays NULL, `zc == 0`,
  `binding == NULL` -> `-EINVAL` at the dmabuf gate.
- `MSG_ZEROCOPY` in `sendmsg` flags and `size != 0`; otherwise same
  `-EINVAL`.
- cmsg `SOL_SOCKET / SCM_DEVMEM_DMABUF`, payload exactly `u32`
  (`sock.c:3194-3198@7.4 / 3123@7.2.15 / 3113@7.2.6`; wrong length ->
  `-EINVAL` from `__sock_cmsg_send`), stored in `sockc->dmabuf_id`
  (`include/net/sock.h:1934@7.4 / 1930@7.2.x`).
- `iov_base` is a **byte offset into the dmabuf**, `iov_len` the length
  (`datagram.c:718-731`, devmem.rst:296-301). Iterator must be `ITER_IOVEC`
  or `ITER_UBUF`, else `-EFAULT`.
- Only TCP implements this (`tcp_sendmsg_locked`). devmem.rst also
  recommends `SO_BINDTODEVICE`.

### `tcp_sendmsg_locked` (`net/ipv4/tcp.c:1122@7.4`, `1116@7.2.15 == @7.2.6`)

v7.4-rc3 `tcp.c:1146-1195` [V]:

```c
	if ((flags & MSG_ZEROCOPY) && size) {
		if (msg->msg_ubuf) {
			...
		} else if (sock_flag(sk, SOCK_ZEROCOPY)) {
			skb = tcp_write_queue_tail(sk);
			uarg = msg_zerocopy_realloc(sk, size, skb_zcopy(skb),
						    !sockc_err && sockc.dmabuf_id);
			if (!uarg) {
				err = -ENOBUFS;
				goto out_err;
			}
			if (sk->sk_route_caps & NETIF_F_SG)
				zc = MSG_ZEROCOPY;
			else
				uarg_to_msgzc(uarg)->zerocopy = 0;

			if (!sockc_err && sockc.dmabuf_id) {
				binding = net_devmem_get_binding(sk, sockc.dmabuf_id);
				if (IS_ERR(binding)) {
					err = PTR_ERR(binding);
					binding = NULL;
					goto out_err;
				}
			}
		}
	}
	...
	if (sockc.dmabuf_id && (zc != MSG_ZEROCOPY || !binding)) {
		err = sockc_err ?: -EINVAL;
		goto out_err;
	}
```

v7.2.15 / v7.2.6 `tcp.c:1140-1175` is the same except the gate is
`if (!sockc_err && sockc.dmabuf_id && (zc != MSG_ZEROCOPY || !binding))
{ err = -EINVAL; ...}` (`:1172`) and there are no "fastopen + binding" /
"repair + binding" `-EINVAL` guards (7.4 `:1186-1189`, `:1213-1216`). [V]

Errors a devmem `sendmsg` can return before any data is queued [V]:

| Condition | errno | Where |
|---|---|---|
| no `MSG_ZEROCOPY`, no `SO_ZEROCOPY`, or `size == 0` with dmabuf cmsg | `-EINVAL` | gate `:1178@7.4 / :1172@7.2.x` |
| **`sk->sk_route_caps` lacks `NETIF_F_SG`** | `-EINVAL` | same gate (`zc == 0`) |
| uarg alloc failed | `-ENOBUFS` | `:1156@7.4` |
| unknown id / RX binding (no `tx_vec`) / (7.4 only) other netns or uid | `-EINVAL` | `devmem.c:408-412@7.4 / :386-388@7.2.x` |
| no route and rebuild failed | `-EHOSTUNREACH` | `devmem.c:420@7.4 / :396@7.2.x` |
| route's device is neither `binding->dev` nor `binding->vdev` | `-ENODEV` | `devmem.c:434-438@7.4 / :410-414@7.2.x` |
| offset >= dmabuf size, or bad iter type | `-EFAULT` | `devmem.c:457@7.4`, `datagram.c:722-733` |
| (7.4 only) MSG_FASTOPEN/DEFER_CONNECT or repair mode | `-EINVAL` | `:1186`, `:1213` |

`msg_zerocopy_realloc(..., devmem=true)` skips `mm_account_pinned_pages()`
(no RLIMIT_MEMLOCK charge) (`skbuff.c:1740,1793@7.4 / 1744,1797@7.2.x`). [V]

Building skbs [V]:

- A devmem send never appends to a readable skb and vice versa:
  `unlikely(skb_frags_readable(skb) != !binding)` forces `new_segment`
  (`tcp.c:1257@7.4 / 1243@7.2.x`).
- `skb_zerocopy_iter_stream()` -> `__zerocopy_sg_from_iter()` ->
  `zerocopy_fill_skb_from_devmem()` when `binding` is set
  (`datagram.c:760-761`). `-EMSGSIZE`/`-EEXIST` -> push and start a new skb
  (`tcp.c:1355@7.4 / 1341@7.2.x`).

### `zerocopy_fill_skb_from_devmem` (`net/core/datagram.c:706-747`, same lines in all three; only `get_netmem` -> `netmem_inc_ref` renamed in 7.4) [V]

```c
	if (i && skb_frags_readable(skb))
		return -EFAULT;
	...
	while (length && iov_iter_count(from)) {
		if (i == MAX_SKB_FRAGS)
			return -EMSGSIZE;

		virt_addr = (size_t)iter_iov_addr(from);
		niov = net_devmem_get_niov_at(binding, virt_addr, &off, &size);
		if (!niov)
			return -EFAULT;

		size = min_t(size_t, size, length);
		size = min_t(size_t, size, iter_iov_len(from));

		netmem_inc_ref(net_iov_to_netmem(niov));
		skb_add_rx_frag_netmem(skb, i, net_iov_to_netmem(niov), off,
				       size, PAGE_SIZE);
		iov_iter_advance(from, size);
		length -= size;
		i++;
	}
```

with `net_devmem_get_niov_at()` (`devmem.c:453-464@7.4 / 429-440@7.2.x`):
`*off = virt_addr % PAGE_SIZE; *size = PAGE_SIZE - *off; return
binding->tx_vec[virt_addr / PAGE_SIZE];`.

What the driver therefore sees:

- Payload lives **only in frags**; the linear area holds just headers (the
  zerocopy branch never copies to linear). [V for the branch; I that headlen
  == header length for every such skb]
- Every frag is exactly one net_iov: `frag->netmem` is a `net_iov`,
  `0 <= skb_frag_off < PAGE_SIZE`, `1 <= skb_frag_size <= PAGE_SIZE - off`.
  Frags never span a page and **adjacent niovs are never coalesced**, even
  when DMA-contiguous. No alignment guarantee on offset or size: they follow
  the user's iov (and `size_goal`/MSS cuts). A user iovec smaller than a page
  gives sub-page frags; an offset not page aligned gives a short first frag.
- At most `MAX_SKB_FRAGS` frags per skb (`CONFIG_MAX_SKB_FRAGS`, default 17,
  range 17..45, `net/Kconfig:292-295@7.4`); this path does not consult
  `sysctl_max_skb_frags`. So a devmem TSO skb carries at most
  17 * 4096 = 69632 payload bytes with the default config, less if iovs are
  small.
- `skb->unreadable = 1` is set by `__skb_fill_netmem_desc()` when the netmem
  is a net_iov (`include/linux/skbuff.h:2609-2610@7.4 / 2600-2601@7.2.15 /
  2617-2618@7.2.6`). `unreadable` is inside the `headers` struct group
  (`skbuff.h:1039@7.4 / 1033@7.2.x`), so `__copy_skb_header()` propagates it
  to clones, copies and GSO segments.
- `shinfo->flags` has `SKBFL_ZEROCOPY_FRAG | SKBFL_DONT_ORPHAN`
  (`skbuff.c:1750@7.4 / 1754@7.2.x`) and usually `SKBFL_PURE_ZEROCOPY`;
  `SKBFL_ZEROCOPY_FRAG` includes `SKBFL_SHARED_FRAG`
  (`skbuff.h:533@7.4 / 527@7.2.x`), so `skb_has_shared_frag()` is true.
- `skb->ip_summed == CHECKSUM_PARTIAL` (normal TCP) and the skb is GSO
  whenever it exceeds one MSS.
- `skb->pp_recycle == 0`; frag refs are binding refs.

### `sk_setup_caps` and the NETIF_F_SG question -- settled

`net/core/sock.c:2669-2700@7.4 / 2640-2671@7.2.15 / 2636-2667@7.2.6`,
function text identical in all three [V]:

```c
	sk->sk_route_caps = dev->features;
	if (sk_is_tcp(sk)) {
		...
		sk->sk_route_caps |= NETIF_F_GSO;
		...
	}
	if (sk->sk_route_caps & NETIF_F_GSO)
		sk->sk_route_caps |= NETIF_F_GSO_SOFTWARE;
	if (unlikely(sk->sk_gso_disabled))
		sk->sk_route_caps &= ~NETIF_F_GSO_MASK;
	if (sk_can_gso(sk)) {
		if (dst->header_len && !xfrm_dst_offload_ok(dst)) {
			sk->sk_route_caps &= ~NETIF_F_GSO_MASK;
		} else {
			sk->sk_route_caps |= NETIF_F_SG | NETIF_F_HW_CSUM;
```

`sk_can_gso()` = `net_gso_ok(sk->sk_route_caps, sk->sk_gso_type)`
(`include/net/sock.h:2318@7.4 / 2302@7.2.x`); `sk_gso_type` is
`SKB_GSO_TCPV4`/`TCPV6` (`tcp_ipv4.c:324`, `tcp_ipv6.c:314 @7.4`);
`NETIF_F_GSO_SOFTWARE` contains `NETIF_F_ALL_TSO`
(`include/linux/netdev_features.h:209-215`).

**Result (same in all three versions):** for an ordinary TCP socket
`sk_route_caps` *always* contains `NETIF_F_SG | NETIF_F_HW_CSUM`, whatever
`dev->features` says. The `sk->sk_route_caps & NETIF_F_SG` test in
`tcp_sendmsg_locked` therefore passes and **sendmsg does not fail** because
the device lacks SG. The reader who said "sendmsg succeeds because
sk_setup_caps forces SG|HW_CSUM" is right for the normal case, in all three
versions.

The `-EINVAL at tcp.c` reader is right only when `sk_can_gso()` is false:

- `sk->sk_gso_disabled` set by `sk_gso_disable()`: TCP-MD5
  (`tcp_ipv4.c:1340,1756`, `tcp_output.c:1724@7.4 / 1668@7.2.x`) and TCP-AO
  (`tcp_ao.c:1321,1722,2023`);
- an xfrm dst with `header_len` that is not offloaded.

Then `sk_route_caps = dev->features & ~NETIF_F_GSO_MASK`, and if the device
has no `NETIF_F_SG`, `zc == 0` and the gate returns `-EINVAL`
(`tcp.c:1178-1180@7.4`, `1172-1174@7.2.x`). If the device does have SG, the
send proceeds with non-GSO (MSS-sized) unreadable skbs.

What happens after sendmsg succeeds is decided in `validate_xmit_skb()`
(section 4). Per device feature set, for a normal (GSO-capable) TCP socket
on a device with `netmem_tx == NETMEM_TX_DMA`:

| dev->features | sendmsg | skb > 1 MSS (GSO) | skb <= 1 MSS | userspace sees |
|---|---|---|---|---|
| **no SG, no csum** | success | `harmonize_features` strips CSUM+GSO (`dev.c:3845-3848@7.4 / 3790-3793@7.2.15 / 3748-3751@7.2.6`) -> `netif_needs_gso` -> `skb_segment()` with `sg=0, csum=0` -> payload produced by `skb_copy_and_csum_bits()` (`skbuff.c:5073@7.4 / 4987@7.2.15 / 4981@7.2.6`). **7.4 / 7.2.15: segments carry all-zero payload** and the TCP checksum is computed over those zeros, so they are valid on the wire. **7.2.6: segments carry uninitialised kmalloc bytes** with a checksum computed as if payload were zero. | `skb_needs_linearize` -> `__skb_linearize` -> `__pskb_pull_tail` returns NULL -> `kfree_skb`, `tx_dropped++` | 7.4/7.2.15: **silent data corruption** (peer receives zeros) for GSO skbs, silent stall for sub-MSS skbs. 7.2.6: kernel heap leak on the wire + almost certain checksum failure at the peer -> retransmit forever -> stall |
| **SG only (no csum for this protocol)** | success | CSUM+GSO stripped, SG kept -> `skb_segment()` `sg=1, csum=0` -> `perform_csum_check`: `skb_has_shared_frag(nskb) && __skb_linearize(nskb)` fails (`skbuff.c:5184@7.4 / 5078@7.2.15 / 5072@7.2.6`) -> `ERR_PTR(-ENOMEM)` -> `kfree_skb`, `tx_dropped++` | `skb_csum_hwoffload_help` -> `skb_checksum_help` -> `-EFAULT` -> `kfree_skb`, `tx_dropped++` | **silent stall**: sendmsg returned success, every (re)transmission is dropped locally, connection eventually dies with ETIMEDOUT |
| **SG + csum, no TSO** | success | `netif_needs_gso` -> `skb_segment()` `sg=1, csum=1`: headers copied, net_iov frags shared by reference (`skb_frag_ref`), uarg shared (`skb_zerocopy_clone`), `unreadable` inherited; segments stay `CHECKSUM_PARTIAL` | passes | **works**; driver gets MSS-sized unreadable skbs |
| SG + csum + TSO | success | passed to driver as one TSO skb | passes | works |

[V] for each individual code step cited; [I] for the end-to-end outcomes
(not run). The zero-payload/valid-checksum conclusion rests on:
`skb_copy_and_csum_bits()` returning 0 after `memset(to, 0, len)`
(`skbuff.c:3659-3664@7.4`, `3636-3641@7.2.15`; `3636-3637@7.2.6` has no
memset), that value stored in `SKB_GSO_CB(nskb)->csum`, and
`tcp_gso_segment()` computing `th->check = gso_make_checksum(skb,
~th->check)` for non-PARTIAL segments (`tcp_offload.c:215,260`;
`include/net/gso.h:75-85@7.4`).

Notes on the feature combinations:

- `netdev_fix_features()` only enforces TSO => SG, TSO => csum, GSO => SG
  (`dev.c:11181-11222@7.4`); it does **not** drop SG when csum is absent, so
  "SG only" is a reachable state, e.g. `ethtool -K <dev> tx off`. [V]
- "csum" means `can_checksum_protocol(features, proto)`: with only
  `NETIF_F_IP_CSUM`, IPv6 flows fall in the "SG only" row; with
  `NETIF_F_IPV6_CSUM` (not `HW_CSUM`), IPv6 packets with extension headers
  take `sw_checksum` in `skb_csum_hwoffload_help` and are dropped
  (`dev.c:4121-4133@7.4`). [V]
- The core does **not** tie `netmem_tx` to features. Nothing stops a user
  turning SG or TX csum off at runtime on a `NETMEM_TX_DMA` device; the
  outcomes above then apply. A driver that wants a clean failure must handle
  this itself (e.g. `ndo_fix_features`/`ndo_set_features` policy). [I]
- If `netmem_tx == NETMEM_TX_NONE` none of this is reachable from devmem
  sendmsg: bind fails with `-EOPNOTSUPP`, and a stray unreadable skb is
  dropped first thing in `validate_xmit_skb`.

### GSO/TSO interaction summary [V/I]

- TSO-capable device: one unreadable skb, linear = Ethernet/IP/TCP headers,
  up to `MAX_SKB_FRAGS` net_iov frags, `gso_size` = MSS. MSS boundaries fall
  anywhere inside frags; the device/driver does the split. [I]
- A driver TSO path that must touch headers may `skb_cow_head()`:
  `pskb_expand_head()` has no unreadable check; `skb_orphan_frags()` is a
  no-op because of `SKBFL_DONT_ORPHAN` (`skbuff.h:3430-3437@7.4`), frags are
  re-referenced with `skb_frag_ref`. Works. [V]
- TCP-internal splitting (`tso_fragment`/`tcp_fragment` -> `skb_split`)
  shares frags by reference and copies `unreadable`
  (`skbuff.c:4239,4288@7.4 / 4181,4230@7.2.15 / 4175,4224@7.2.6`). Collapse
  on retransmit is refused: `tcp_can_collapse()` returns false for
  unreadable skbs (`tcp_output.c:3561@7.4 / 3497@7.2.x`);
  `skb_try_coalesce` and `tcp_skb_can_collapse` require equal readability
  (`skbuff.c:6316@7.4`; `include/net/tcp.h:1271@7.4 / 1239@7.2.15 /
  1240@7.2.6`). [V]

## 4. Everything that rejects, drops or cannot handle an unreadable skb

`skb_frags_readable(skb)` is `!skb->unreadable`
(`skbuff.h:1880@7.4 / 1874@7.2.15 / 1891@7.2.6`). Line numbers `7.4 /
7.2.15 / 7.2.6`. Unless noted, function bodies are identical in all three.

### TX validation (`net/core/dev.c`)

`validate_xmit_unreadable_skb()` `4169 / 4048 / 3991` (identical) [V]:

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
	...
out_free:
	kfree_skb(skb);
	return NULL;
```

Called first in `validate_xmit_skb()` (`4206 / 4085 / 4028`); NULL ->
`dev_core_stats_tx_dropped_inc(dev)` (`4273 / 4136 / 4079`). Drop reason is
plain `kfree_skb()` = `SKB_DROP_REASON_NOT_SPECIFIED`. No errno reaches the
socket: TCP sees the segment as sent, retransmits on RTO, and it is dropped
again -> **silent stall** until `tcp_retries2` (ETIMEDOUT). Only the first
frag is checked, and only for devmem niovs.

Triggers: route moved to another netdev after the per-sendmsg
`net_devmem_get_binding()` check (queued/retransmitted data); stacked
devices (the skb passes `validate_xmit_skb` on the upper dev, e.g. vlan,
bond, macvlan, whose `netmem_tx` is `NETMEM_TX_NONE`). In the normal case
those are refused earlier and cleanly: bind on the upper dev ->
`-EOPNOTSUPP`; send with a binding made on the lower dev -> `-ENODEV`
(dst dev != `binding->dev`). [I]

### Table

| Function | Lines (7.4 / 7.2.15 / 7.2.6) | Behaviour on unreadable skb | Effect on devmem TX |
|---|---|---|---|
| `validate_xmit_unreadable_skb` | dev.c 4169 / 4048 / 3991 | `kfree_skb`, NULL | silent drop/stall (above) |
| `skb_checksum_help` | dev.c 3679 / 3624 / 3582 | `return -EFAULT` (before the shared-frag linearize) | via `skb_csum_hwoffload_help` in `validate_xmit_skb` -> `kfree_skb`, tx_dropped; silent stall. Also any driver calling it on its xmit path |
| `skb_csum_hwoffload_help` | dev.c 4111 / 3990 / 3932 | falls to `skb_checksum_help` unless HW_CSUM, or IP/IPV6_CSUM with TCP/UDP offset and no IPv6 ext hdr (7.2.6 tests `skb_network_header_len != sizeof(ipv6hdr)` instead of `skb_has_ipv6_extension_hdr`) | as above |
| `__pskb_pull_tail` | skbuff.c 2906 / 2886 / 2886 | `return NULL` | callers see failure: `pskb_may_pull` beyond headlen false, `__skb_linearize` -> `-ENOMEM` |
| `__skb_linearize` / `skb_linearize` | skbuff.h 4023 / 4011 / 4023 | `-ENOMEM` (via `__pskb_pull_tail`) | `skb_needs_linearize` in `validate_xmit_skb` (no SG / no FRAGLIST) -> drop; **driver "too many frags -> skb_linearize" fallbacks fail** |
| `skb_copy_bits` | skbuff.c 3062 / 3042 / 3042 | linear part copied, then `-EFAULT` | `skb_segment` `!sg && csum` -> `ERR_PTR(-ENOMEM)` -> drop; bpf `skb_load_bytes` past headlen fails |
| `skb_copy_and_csum_bits` | skbuff.c 3659 / 3636 / 3636 | 7.4, 7.2.15: zero-fill rest, return 0. 7.2.6: return 0, rest untouched | `skb_segment` `!sg && !csum`: zeros (or stale heap on 7.2.6) go on the wire; also `skb_copy_and_csum_dev()` bounce paths and ICMP quoting |
| `skb_checksum` | skbuff.c 3570 / 3550 / 3550 | `WARN_ON_ONCE`, return 0 | wrong checksum + one-time splat for any sw checksum over payload (e.g. `act_csum`, `skb_segment` `!csum` on an all-linear seg is fine) |
| `skb_crc32c` | skbuff.c 3751 / 3722 / 3718 | `WARN_ON_ONCE`, return 0 | n/a for TCP |
| `skb_copy` | skbuff.c 2189 / 2186 / 2186 | `return NULL` | callers treat as ENOMEM |
| `skb_copy_expand` | skbuff.c 2531 / 2516 / 2516 | `return NULL` | same |
| `skb_copy_ubufs` | skbuff.c 2010 / 2007 / 2007 | `return -EFAULT` | reached through `skb_orphan_frags_rx()` (always) and `skb_orphan_frags()` (not for MSG_ZEROCOPY uargs: `SKBFL_DONT_ORPHAN`) |
| `skb_store_bits` | skbuff.c 3490 / 3470 / 3470 | `-EFAULT` | payload rewrite (bpf, nat helpers) fails |
| `skb_seq_read` | skbuff.c 4539 / 4470 / 4464 | returns 0 (end) | string/bm matching sees headers only |
| `__skb_splice_bits` | skbuff.c 3251 / 3231 / 3231 | stops at frags | RX side |
| `skb_condense` | skbuff.c 7255 / 7112 / 7068 | no-op | harmless |
| `skb_zerocopy` | skbuff.c 3952 (7.4 only) | `-EFAULT` if it would have to mix a copied head frag with unreadable frags; sets `to->unreadable` | 7.2.x has no check here (nfqueue/ovs upcall copy path guarded by their own length clamps) |
| `__skb_datagram_iter` | datagram.c 411 (all) | `goto short_copy` -> `-EFAULT` | RX/recvmsg copy |
| `zerocopy_fill_skb_from_iter` | datagram.c 640 (all) | `-EFAULT` | cannot append page zerocopy to an unreadable skb |
| `zerocopy_fill_skb_from_devmem` | datagram.c 715 (all) | `-EFAULT` if skb already has readable frags | never mixes |
| `tcp_can_collapse` | tcp_output.c 3561 / 3497 / 3497 | false | no retransmit collapse |
| `pskb_expand_head`, `skb_cow_head`, `skb_clone`, `__pskb_copy_fclone`, `skb_split`, `skb_shift` | -- | **no unreadable check**; only head is copied, frags shared by ref | work |
| `skb_cow` / `skb_cow_data` on payload | -- | no explicit check; any path that needs to copy/modify frag data ends in one of the failing helpers above | fail |
| `skb_segment` (software GSO) | skbuff.c 4857 / 4785 / 4779 | **no unreadable check**; behaviour depends on `sg`/`csum` (section 3 table) | works only with SG+csum; otherwise drop, or zero/garbage payload when neither |
| `nfulnl_get_copy_len` / `nfqnl_get_data_len` | nfnetlink_log.c 715 / 715 / 710; nfnetlink_queue.c 723 / 705 / 698 | clamp to headlen | NFLOG/NFQUEUE see headers only |
| OVS `queue_userspace_packet` | openvswitch/datapath.c 471 (7.4 only) | clamps upcall to headlen | 7.2.x: no clamp |
| `packet_rcv` / `tpacket_rcv` | af_packet.c 2182,2302 / 2174,2294 / 2174,2294 | `snaplen = skb_headlen(skb)` | applies to unreadable skbs that reach af_packet (RX devmem) |

All [V] for the check itself; "Effect" column is [I].

### Taps: tcpdump / `dev_queue_xmit_nit` [V]

`dev.c:2620-2683@7.4 / 2571@7.2.15 / 2549@7.2.6` (identical). The tap gets a
clone, but before delivery:

```c
	if (pt_prev) {
		if (!skb_orphan_frags_rx(skb2, GFP_ATOMIC))
			pt_prev->func(skb2, skb->dev, pt_prev, skb->dev);
		else
			kfree_skb(skb2);
	}
```

`skb_orphan_frags_rx()` calls `skb_copy_ubufs()` for any zerocopy skb,
ignoring `SKBFL_DONT_ORPHAN` (`skbuff.h:3440-3445@7.4`), which returns
`-EFAULT` for unreadable skbs. So for devmem **TX** skbs the clone is
freed: the last tap gets nothing (earlier taps in the list go through
`deliver_skb()`, which has the same orphan step). The original skb is
untouched and is still transmitted. Result: **tcpdump on the sender shows no
devmem data packets at all** (not even headers); no error, no stall. This
matters for debugging the PoC: capture on the receiver or in the emulator.
[V code, I conclusion]

### tc / qdisc [I, from grep of payload-touching helpers at 7.4]

- Default `fq`/`fq_codel`/`pfifo_fast`, `mq`: header-only; fine.
- `tbf`, `taprio`, `cake`, `netem` segment with
  `skb_gso_segment(skb, features & ~NETIF_F_GSO_MASK)`
  (`sch_tbf.c:219`, `sch_taprio.c:598`, `sch_cake.c:1811`,
  `sch_netem.c:443`): keeps SG+csum, so segmentation shares frags; fine.
- `netem` corruption: `skb_linearize(skb) || skb_checksum_help(skb)` ->
  `qdisc_drop` (`sch_netem.c:537-538`, same line in all three): packet
  dropped, silent.
- tc/cls BPF and actions: direct packet access is limited to the linear
  area; `bpf_skb_load_bytes` past headers -> `-EFAULT`,
  `bpf_skb_pull_data` -> `-ENOMEM`; `act_csum` over payload -> `skb_checksum`
  WARN + wrong checksum; `act_mirred` clones (fine on egress to the same
  dev, dropped by `validate_xmit_unreadable_skb` on another dev).
- Generic XDP is receive-side only; not on this TX path. Loopback / veth /
  redirect-to-rx paths need `skb_orphan_frags_rx` and so fail (devmem.rst:
  "Loopback is not functional").
- Bonding/team/vlan/macvlan/bridge as the route's device: bind refused
  (`-EOPNOTSUPP`, their `netmem_tx` is NONE); send with a lower-dev binding
  refused (`-ENODEV`). Clean errors.

### Clean error vs silent stall

- **Clean errno to userspace** only from `sendmsg` itself (section 3 table)
  and from bind.
- **Everything after the skb is queued is silent**: drops in
  `validate_xmit_skb` bump `tx_dropped` only. TCP retransmits, hits the same
  drop, and the socket eventually errors with ETIMEDOUT. The zerocopy
  completion for the affected range is still delivered when the skbs are
  finally purged (`skb_zcopy_clear(skb, true)` in `skb_release_data` reports
  success irrespective of the reason the skb was freed). [V for the call; I
  for the overall behaviour]

## 5. Driver-side DMA helpers

### `skb_frag_dma_map()` on a net_iov frag [V]

v7.4-rc3 `include/linux/skbuff.h:3823-3837`:

```c
	if (skb_frag_is_net_iov(frag)) {
		addr = netmem_dma_addr_decode(
			netmem_get_dma_addr(frag->netmem));
		return addr + offset + frag->offset;
	}
	return dma_map_page(dev, skb_frag_page(frag),
			    skb_frag_off(frag) + offset, size, dir);
```

v7.2.15 `skbuff.h:3814-3825`, v7.2.6 `skbuff.h:3826-3837`:

```c
	if (skb_frag_is_net_iov(frag)) {
		return netmem_to_net_iov(frag->netmem)->desc.dma_addr +
		       offset + frag->offset;
	}
```

Same result on 64-bit. No DMA API call is made, `dev`, `size` and `dir` are
ignored, and **it cannot fail** for a net_iov (a `dma_mapping_error()` check
on the result is still harmless). The returned address is the bind-time
mapping for `binding`'s `dma_dev`. A driver that already uses
`skb_frag_dma_map()` (ionic does: `ionic_txrx.c:1110@7.4`) needs no change on
the map side. Anything that derives a page or kernel address from the frag
instead (`skb_frag_page()` returns NULL for net_iov, `skb_frag_address()`,
`kmap`) is broken for these frags. No `dma_sync_*` may be done on the
address.

### Unmap helpers (`include/net/netmem.h:447-464@7.4`, `397-414@7.2.15 == @7.2.6`; identical text) [V]

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

Contract: at map time record **0** as the unmap address for a net_iov frag;
at completion `netmem_dma_unmap_page_attrs()` skips address 0. The helper
does not look at the skb or frag at unmap time, so the "is net_iov" decision
is made when the address is stored. (Implicit assumption: 0 is never a real
DMA address for a page frag.)

`dma_unmap_addr*` semantics (`include/linux/dma-mapping.h:785-803@7.4 /
777@7.2.x`) [V]: with `CONFIG_NEED_DMA_MAP_STATE` the macros are plain struct
field accessors. Without it, `DEFINE_DMA_UNMAP_ADDR` declares nothing,
`dma_unmap_addr_set()` is a no-op and `dma_unmap_addr()` evaluates to `0`.
So `netmem_dma_unmap_page_attrs(dev, dma_unmap_addr(...), ...)` returns early
for **every** frag in such a config, which is correct only because those
configs need no unmap. x86 selects `NEED_DMA_MAP_STATE`
(`arch/x86/Kconfig:33@7.4`), as do IOMMU_DMA/SWIOTLB/DMA_DEBUG
(`kernel/dma/Kconfig`), so on the test VMs the field is real.

Consequence for a driver that keeps its own `dma_addr_t` per buffer rather
than `DEFINE_DMA_UNMAP_ADDR` (ionic: `buf_info->dma_addr`,
`dma_unmap_page()` at `ionic_txrx.c:1157,1179@7.4`): it still needs the
descriptor's real DMA address at map time, but must make its unmap path skip
net_iov frags -- either a separate "unmap address" set through
`netmem_dma_unmap_addr_set()` and unmapped with
`netmem_dma_unmap_page_attrs()`, or a per-buffer flag. Calling
`dma_unmap_page()` on a net_iov address is the bug netmem.rst rule 1 forbids
(with an IOMMU it would tear down part of the dmabuf's mapping). [I]

## 6. Documentation rules

`Documentation/networking/netmem.rst`, "Driver TX Requirements"
(`:134-157@7.4`, `:81-104@7.2.15 == @7.2.6`; text identical) [V]:

1. "The Driver must not pass the netmem dma_addr to any of the dma-mapping
   APIs directly. This is because netmem dma_addrs may come from a source
   like dma-buf that is not compatible with the dma-mapping APIs. Helpers
   like netmem_dma_unmap_page_attrs() & netmem_dma_unmap_addr_set() should be
   used in lieu of dma_unmap_page[_attrs](), dma_unmap_addr_set()."
2. "Driver should declare support by setting `netdev->netmem_tx` to the
   appropriate mode: `NETMEM_TX_DMA`: for physical devices that perform DMA.
   `NETMEM_TX_NO_DMA`: for virtual or passthrough devices that do not DMA,
   but still support handling of netmem-backed skbs."

That is the complete documented TX contract. 7.4 adds a "Design Principles"
section (`:22-73@7.4`, absent in 7.2.x) relevant to drivers: operate on
`netmem_ref`, do not downcast; do not assume net_iov implies unreadable --
test `skb_frags_readable()` / `netmem_address()`; never mix page and
memory-provider frags, or frags of different providers, in one skb.

`Documentation/networking/devmem.rst` (file identical in all three) [V]:
TX interface `:236-345` (bind via netlink, unbind by closing the netlink
socket, `SO_ZEROCOPY`, recommended `SO_BINDTODEVICE`, `SCM_DEVMEM_DMABUF`
with `iov_base` = offset, completions via `MSG_ERRQUEUE`, do not modify the
dmabuf until the completion arrives); caveats `:378-388`: "Loopback is not
functional", "Software checksum calculation fails", "TCP Dump and bpf can't
access devmem packet payloads".

Requirements the docs do not state but the code imposes (sections 1-5):

- The bind-time DMA device (`ndo_queue_get_dma_dev` or `dev.parent`) must be
  the device whose DMA addresses the hardware consumes.
- `NETIF_F_SG` and checksum offload for the flow's protocol must be
  **active** (not merely supported) or traffic is dropped / corrupted.
- The xmit path must never linearize, pull, copy, checksum, kmap or
  `skb_frag_address()` the payload of an skb with `!skb_frags_readable()`.
  If the driver has a "too many frags -> `skb_linearize()`" fallback (ionic:
  `ionic_tx_descs_needed()`, `ionic_txrx.c:1695-1774@7.4`), that fallback
  fails with `-ENOMEM` for these skbs and the driver's drop path runs.
- Must cope with up to `MAX_SKB_FRAGS` sub-page frags per skb with arbitrary
  offsets, in both TSO and non-TSO skbs.
- Must free every TX skb through the normal skb free paths on completion and
  on ring teardown.

## 7. Completion path

[V] unless marked; line numbers `7.4 / 7.2.15 / 7.2.6`.

1. Driver TX completion frees its skb (`napi_consume_skb`/`dev_kfree_skb*`).
   For TCP that is a clone; the original stays on the rtx queue until ACKed.
   `skb_release_data()` (`skbuff.c:1101 / 1099 / 1099`) runs only when the
   shared data refcount (`dataref`) drops to zero, i.e. after both the
   driver's clone and TCP's copy are gone:

   ```c
	if (skb_zcopy(skb)) {
		bool skip_unref = shinfo->flags & SKBFL_MANAGED_FRAG_REFS;

		skb_zcopy_clear(skb, true);
		if (skip_unref)
			goto free_head;
	}

	for (i = 0; i < shinfo->nr_frags; i++)
		skb_netmem_unref(skb_frag_netmem(&shinfo->frags[i]),
				 skb->pp_recycle);
   ```
   (7.2.x: `__skb_frag_unref(&shinfo->frags[i], skb->pp_recycle)`.)
2. `skb_zcopy_clear(skb, true)` -> `uarg->ops->complete()` =
   `msg_zerocopy_complete()` (`skbuff.c:1883 / 1887 / 1887`):
   `refcount_dec_and_test(&uarg->refcnt)` -> `__msg_zerocopy_callback()`
   (`1833 / 1837 / 1837`), which queues (or merges into the tail of)
   an `SO_EE_ORIGIN_ZEROCOPY` notification on `sk->sk_error_queue` with
   `ee_info = lo`, `ee_data = hi` (range of sendmsg call counters, starting at
   0 per socket), then `sk_error_report(sk)`. Userspace reads it with
   `recvmsg(MSG_ERRQUEUE)`. `SO_EE_CODE_ZEROCOPY_COPIED` is not set for
   devmem (`uarg->zerocopy` stays 1 because `sk_route_caps` has SG).
   One uarg can cover several sendmsg calls (up to 2^19 bytes,
   `byte_limit`, `skbuff.c:1767 / 1771 / 1771`) and several skbs; the
   notification fires when the last skb referencing it is freed.
3. Frag refs: `pp_recycle` is 0 for these skbs, so `skb_netmem_unref()` ->
   `netmem_dec_ref()` -> `net_iov_dec_ref()` -> `net_devmem_net_iov_dec_ref()`
   -> `percpu_ref_put(&binding->ref)` (7.4: `skbuff_ref.h:59-66`,
   `netmem.h:437-443`, `skbuff.c:7724-7730`, `devmem.c:394-397`; 7.2.x:
   `skb_page_unref` -> `put_netmem` -> `__put_netmem`
   (`skbuff.c:7546@7.2.15 / 7494@7.2.6`) -> `net_devmem_put_net_iov`).
   **A net_iov has no refcount of its own on TX**: the count is on the
   binding, one per frag reference. There is no per-niov "busy" state and the
   kernel does not stop userspace reusing a dmabuf range before the
   completion; that is the application's contract (devmem.rst "Reusing TX
   dmabufs").
4. When the binding's ref hits zero after unbind,
   `__net_devmem_dmabuf_binding_free()` unmaps and detaches from a workqueue
   (section 2).

Driver obligations for completion: nothing devmem-specific beyond freeing the
skb and **not** unmapping net_iov frags. The notification, uarg and binding
refs are handled entirely by the skb free path. Freeing with
`dev_kfree_skb_any`/`napi_consume_skb` from the ring-cleanup path at
`ndo_stop` is sufficient to release bindings. [I]

## 8. Open questions / not verified by running

1. End-to-end outcomes in the section 3 feature table (zeros on the wire on
   7.2.15/7.4, heap bytes on 7.2.6, stalls) are reasoned from code, not
   observed. Worth one negative test each with `ethtool -K sg off` /
   `tx off` / `tso off` on the PoC.
2. TX binding vs netdev unregister / module unload (stale `binding->dev`,
   possible UAF in `netdev_nl_sock_priv_destroy`) -- inferred from the
   absence of any TX handling in the unregister path in all three tags; not
   reproduced. Avoid by closing the netlink socket before `rmmod`.
3. Whether ionic's `q->max_sg_elems` (and the emulator's SG limit) can carry
   `MAX_SKB_FRAGS` (17) frags + header for TSO and non-TSO skbs without
   hitting the `skb_linearize()` fallback; if not, the driver needs a
   strategy that does not linearize (e.g. advertise limits so the stack
   segments, or drop with a counter).
4. ionic TSO path details (`ionic_tx_tso`, `ionic_txrx.c:849-877@7.4`,
   `skb_cow_head` at `:1358,1382`) operate on headers + per-frag DMA chunks;
   needs a driver-side read to confirm nothing dereferences frag pages.
5. dma-buf exporter behaviour differs by version: 7.2.x static attach (no
   P2P; amdgpu migrates to GTT/system memory per commit 69ab87acc614's
   message), 7.4 dynamic + pinned + P2P. For the DKMS-on-7.2.15 target only
   the 7.2.x behaviour applies; with udmabuf there is no visible difference.
6. 7.4 adds a uid + netns match between the binding netlink socket and the
   sending socket (`devmem.c:408-412@7.4`, commit 3a82195416fd, not in
   7.2.15): test programs that bind as one user and send as another work on
   7.2.x and fail with `-EINVAL` on 7.4.
7. `CONFIG_MAX_SKB_FRAGS` and `CONFIG_NET_DEVMEM` values of the actual
   7.2.15 guest kernel were not checked (no config in the trees).

## Verification pass

Independent re-read of the source at the three tags (nothing run). Citation
form as above. Status: CONFIRMED / WRONG / UNVERIFIABLE. "Imprecise" marks a
claim whose substance holds but whose wording overstates.

### Version delta

- CONFIRMED: `devmem.c`, `devmem.h`, `netdev-genl.c`, `datagram.c`,
  `include/net/netmem.h`, `netdev_queues.c`, `netmem.rst` hash-identical
  between 7.2.6 and 7.2.15; `devmem.rst` identical in all three (md5).
- CONFIRMED: `tcp_sendmsg_locked`, `sk_setup_caps`,
  `validate_xmit_unreadable_skb`, `harmonize_features`,
  `dev_queue_xmit_nit`, `skb_segment`, `validate_xmit_skb` function text
  identical 7.2.6 == 7.2.15. `sk_setup_caps`,
  `validate_xmit_unreadable_skb`, `harmonize_features`,
  `dev_queue_xmit_nit` also identical in 7.4.
- CONFIRMED: only devmem-TX-relevant 7.2.6 -> 7.2.15 change is the
  `skb_copy_and_csum_bits()` memset (`skbuff.c:3636-3641@7.2.15` vs
  `3636-3637@7.2.6`, stable commit 39d1a6b60183).
- WRONG (imprecise) in the returned summary, "v7.4-rc3 differs in bind
  internals only". 7.4 also differs in: the sendmsg gate errno
  (`sockc_err ?: -EINVAL`, `tcp.c:1178-1179@7.4`), fastopen/repair `-EINVAL`
  guards (`tcp.c:1186,1213@7.4`), the uid/netns check
  (`devmem.c:408-410@7.4`), `validate_xmit_skb` (partial software GSO via
  `max_segs` when the skb exceeds device GSO limits, `dev.c:4235-4238@7.4`;
  7.2.x calls plain `skb_gso_segment`), and `skb_segment` (BUG_ONs turned
  into `-EINVAL`, `pp_recycle` handling). None changes the driver contract;
  the section-0 table in the notes already lists most of them.
- Note not in the original: the DKMS module runs on the 7.2.15 core, so the
  7.2.6 column of every *core* behaviour (including the uninitialised-heap
  row) never executes in the planned setup; 7.2.6 matters only as the ionic
  source pin.

### netmem_tx / bind

- CONFIRMED: `enum netmem_tx_mode`, `unsigned long netmem_tx:2` in all three
  (`netdevice.h:1822,2180@7.4 / 1811,2166@7.2.15 / 1805,2160@7.2.6`); the
  field is not under any `#ifdef`. No bool compat needed.
- CONFIRMED: `NETMEM_TX_NONE` -> `-EOPNOTSUPP`; `NETMEM_TX_DMA` binds to the
  device itself; `NETMEM_TX_NO_DMA` redirects to a leased physical device
  (`netdev-genl.c:1163-1187,1241-1254@7.4`, `1129-1153,1201-1214@7.2.x`).
- CONFIRMED: BIND_TX has no admin-perm flag in 7.2.x either
  (`netdev-genl-gen.c:233-238@7.2.15`, `.flags = GENL_CMD_CAP_DO`).
- CONFIRMED: DMA device = `ndo_queue_get_dma_dev` else `dev->dev.parent`,
  must have `dma_mask` (`netdev_queues.c:9-21`), queue index 0, TX type takes
  the non-lease branch (`netdev_queues.c:46-47@7.4`).
- CONFIRMED, and relevant to "driver-only": bind takes the plain netdev
  instance lock via `netdev_get_by_index_lock()` (`dev.c:1163-1172@7.4`);
  it does **not** require `request_ops_lock`, `queue_mgmt_ops`, or any other
  driver opt-in besides `netmem_tx`. ionic has none of those and needs none.
- CONFIRMED: 7.2.x static `dma_buf_attach` + `map_attachment_unlocked`
  (`devmem.c:229-237@7.2.x`); 7.4 `dma_buf_dynamic_attach` + `dma_buf_pin` +
  `dma_resv_wait_timeout` (`devmem.c:237-266@7.4`); `allow_peer2peer = true`
  with no `move_notify` is accepted (`dma-buf.c:1052-1054@7.4`);
  `dma_buf_pin` is a no-op without `ops->pin` (`dma-buf.c:1143-1146@7.4`).
- CONFIRMED: alignment: 7.2.x checks dmabuf size (TX only) and sg len
  (`devmem.c:245,276@7.2.x`); 7.4 checks size, sg dma_addr and len, `-E2BIG`,
  `-EFAULT` (`devmem.c:268-331@7.4`).
- CONFIRMED: direction `DMA_TO_DEVICE`; one net_iov per PAGE_SIZE; address
  precomputed at bind; `tx_vec` indexed by dmabuf offset / PAGE_SIZE.
- CONFIRMED: `binding->dev` raw pointer, no `netdev_hold`
  (`devmem.c:219@7.4 / 214@7.2.x`).
- CONFIRMED: no unbind netlink op (`uapi/linux/netdev.h:228-243@7.2.15`);
  unbind only from `netdev_nl_sock_priv_destroy`
  (`netdev-genl.c:1470-1495@7.4 / 1425-1450@7.2.x`).
- CONFIRMED (code), INFERRED (consequence) stays INFERRED: nothing in
  unregister touches a TX binding. `binding->dev` is written only at
  `devmem.c:219` and `:558@7.4` (RX uninstall); the bindings xarray is only
  touched by alloc/load/erase (`devmem.c:128,342,379@7.4`);
  `netdev_genl_netdevice_event` handles only notifications
  (`netdev-genl.c:1497-1525@7.4`). I found no counter-evidence; the stale
  pointer and `netdev_hold()` on freed memory at socket close follow from
  the code but were not reproduced.
- Addition: `CONFIG_NET_DEVMEM` is `def_bool y` depending only on
  `DMA_SHARED_BUFFER` and `PAGE_POOL` (`net/Kconfig:69-73@7.2.15`); no
  `.config` in the trees, so the guest value is still UNVERIFIABLE here.

### sendmsg / frags

- CONFIRMED: cmsg length check, `sockc.dmabuf_id`, only TCP consumes it
  (`sock.c:3194-3198@7.4 / 3123-3127@7.2.15`; sole users `tcp.c:1154-1178@7.4`).
- CONFIRMED: gate text and errnos per version as quoted
  (`tcp.c:1146-1181@7.4`, `1140-1175@7.2.x`).
- CONFIRMED: `net_devmem_get_binding` errnos: `-EINVAL`, `-EHOSTUNREACH`,
  `-ENODEV` (`devmem.c:399-451@7.4 / 377-427@7.2.x`); uid/netns only in 7.4.
- CONFIRMED: `zerocopy_fill_skb_from_devmem` (`datagram.c:706-747`, all):
  one frag per net_iov, size `<= PAGE_SIZE - off`, never coalesced,
  `-EMSGSIZE` at `MAX_SKB_FRAGS`; `CONFIG_MAX_SKB_FRAGS` default 17, range
  17..45 (`net/Kconfig:292-295@7.4`).
- CONFIRMED: later TCP manipulation cannot merge or grow frags:
  `skb_shift` bails on zcopy skbs (`skbuff.c:4353@7.4`), `tcp_can_collapse`
  refuses unreadable (`tcp_output.c:3561@7.4`), `skb_split` copies
  `unreadable` (`skbuff.c:4239,4288@7.4 / 4181,4230@7.2.15`). Splits can
  make frags *smaller* than what sendmsg built.
- CONFIRMED: `unreadable` set by `__skb_fill_netmem_desc`
  (`skbuff.h:2610@7.4 / 2601@7.2.15`) and copied by `__copy_skb_header`.
  Detail the notes miss: in the "no SG" software-GSO copy path the produced
  segments are fully linear (no frags, no uarg) yet still carry
  `unreadable = 1`, because `__copy_skb_header` copies the bit
  (`skbuff.c:5056,5068-5083@7.4`). A driver must not treat
  `skb->unreadable` as "has net_iov frags"; test the frag
  (`skb_frag_is_net_iov`).

### SG / checksum dispute

- CONFIRMED: `sk_setup_caps` is byte-identical in the three tags and ORs in
  `NETIF_F_GSO` for every TCP socket, then `NETIF_F_GSO_SOFTWARE`, then
  `NETIF_F_SG | NETIF_F_HW_CSUM` when `sk_can_gso()`
  (`sock.c:2676-2691@7.4`). So the `sk_route_caps & NETIF_F_SG` test
  (`tcp.c:1159@7.4 / 1153@7.2.x`) passes regardless of `dev->features`.
  sendmsg succeeds. The second reader was right.
- CONFIRMED: the `-EINVAL` gate fires for lack of SG only when GSO is off
  for the socket: `sk_gso_disable()` callers are TCP-MD5 and TCP-AO only
  (`tcp_ipv4.c:1340,1756`, `tcp_output.c:1724`, `tcp_ao.c:1321,1722,2023
  @7.4`; same set @7.2.15), or a non-offloaded xfrm dst
  (`sock.c:2688-2689@7.4`). Other writers of `sk_route_caps` only zero it on
  route failure.
- CONFIRMED row "no SG, no csum": `harmonize_features` strips CSUM+GSO
  (`dev.c:3845-3847@7.4`), `skb_segment` `!sg && !csum` ->
  `skb_copy_and_csum_bits` (`skbuff.c:5068-5076@7.4`), zero-fill on 7.4 and
  7.2.15, no fill on 7.2.6; `gso_make_checksum` then produces a valid
  checksum over zeros (`tcp_offload.c:215,260@7.4`). Sub-MSS skbs dropped
  via `skb_needs_linearize` -> `__pskb_pull_tail` NULL
  (`skbuff.c:2906@7.4 / 2886@7.2.x`).
- CONFIRMED row "SG only": GSO skb dies at `perform_csum_check`
  (`skbuff.c:5183-5186@7.4`; `SKBFL_ZEROCOPY_FRAG` includes
  `SKBFL_SHARED_FRAG`, `skbuff.h:533@7.4 / 527@7.2.x`); sub-MSS skb dies in
  `skb_checksum_help` `-EFAULT` (`dev.c:3679-3680@7.4 / 3624@7.2.15 /
  3582@7.2.6`). Drop + `tx_dropped`.
- CONFIRMED row "SG + csum, no TSO": frags shared by reference
  (`skbuff.c:5154-5155@7.4`; 7.2.x `__skb_frag_ref` -> `get_netmem` ->
  binding ref, `skbuff_ref.h@7.2.15`, `skbuff.c:7533-7539@7.2.15`).
- MISSING row (not asked for, but reachable with `ethtool -K sg off` while
  tx csum stays on): **csum without SG**. `netdev_fix_features` drops TSO
  (`dev.c:11191-11194@7.4`); GSO skb -> `skb_segment` `!sg && csum` ->
  `skb_copy_bits` `-EFAULT` -> `goto err` (`skbuff.c:5080-5081@7.4`) -> drop;
  sub-MSS skb -> linearize fails -> drop. Silent stall, not corruption. So
  on a device with HW csum, `sg off` alone stalls; zeros-on-the-wire needs
  both SG and csum off.
- CONFIRMED: core does not couple `netmem_tx` to active features.

### Unreadable-skb table, taps

- CONFIRMED: `validate_xmit_unreadable_skb` text and placement; plain
  `kfree_skb`; only `frags[0]` checked.
- CONFIRMED: `skb_copy_ubufs` returns `-EFAULT` before anything else
  (`skbuff.c:2010-2011@7.4 / 2007-2008@7.2.x`); `skb_orphan_frags_rx`
  ignores `SKBFL_DONT_ORPHAN` (`skbuff.h:3440-3445@7.4 / 3431-3436@7.2.15`);
  `dev_queue_xmit_nit` frees the clone (`dev.c:2676-2681@7.4`). Sender-side
  tcpdump sees no devmem data packets. Exception the notes do not state:
  the zero-filled linear segments of the "no SG, no csum" row have no uarg
  and *are* delivered to taps.
- CONFIRMED: `skb_zerocopy` unreadable handling is 7.4 only
  (`skbuff.c:3983-3984@7.4`; absent @7.2.15 despite the nearby stable
  backport d7d6ff98d5d1).
- Not re-checked line by line (UNVERIFIABLE in this pass, low impact on the
  driver): the nfnetlink/OVS/af_packet rows, `skb_seq_read`,
  `skb_store_bits`, `skb_crc32c`, the tc qdisc list beyond netem
  (`sch_netem.c:537-538@7.4` confirmed).

### Driver helpers, docs, completion

- CONFIRMED: `__skb_frag_dma_map` net_iov branch
  (`skbuff.h:3830-3834@7.4`, `3819-3822@7.2.15`, `3831-3834@7.2.6`); no DMA
  API call, cannot fail on 64-bit (`netmem_dma_addr_decode` is identity
  unless 32-bit arch with 64-bit DMA, `netmem.h:379-385@7.4`).
- CONFIRMED: `netmem_dma_unmap_addr_set` / `netmem_dma_unmap_page_attrs`
  text (`netmem.h:447-464@7.4`), and the `!CONFIG_NEED_DMA_MAP_STATE`
  semantics (`dma-mapping.h:785-803@7.4`).
- CONFIRMED: netmem.rst TX rules 1 and 2 (`netmem.rst:81-104@7.2.15`);
  devmem.rst caveats (`devmem.rst:374-388`).
- CONFIRMED: completion: `skb_release_data` -> `skb_zcopy_clear(skb, true)`
  -> `msg_zerocopy_complete` -> errqueue (`skbuff.c:1101-1119,1883-1891@7.4`);
  frag ref = binding `percpu_ref` (`devmem.c:389-397@7.4 / 367-375@7.2.x`).

### Open question 2 of the summary is answerable from source (changes the patch design)

ionic's TX SG limit is fixed in the driver, not an emulator property:
`qti->max_sg_elems = min_t(u16, max_frags - 1, MAX_SKB_FRAGS)` with
`IONIC_TX_MAX_FRAGS = 1 + IONIC_TX_MAX_SG_ELEMS_V1 = 16`
(`ionic_lif.c:3982-3989@7.4 / 3979-3986@7.2.6`, `ionic_dev.h:213`,
`ionic_if.h:1023@7.4 / 994@7.2.6`). So one descriptor carries the linear
header plus at most **15** frags, while a devmem skb can carry
`MAX_SKB_FRAGS` = 17. `ionic_tx_descs_needed()` then calls
`skb_linearize()` (`ionic_txrx.c:1720-1722,1754-1757,1773-1774@7.4`;
`1714,1748,1766-1767@7.2.6`), which returns `-ENOMEM` for an unreadable skb
(`skbuff.c:2906@7.4`), and `ionic_start_xmit` drops it
(`ionic_txrx.c:1842-1843,1814-1817@7.4`): silent stall.
Reachability [I]: needs 16+ frags in a non-GSO skb, or 16+ buffers inside
one MSS of a TSO skb, i.e. user iovecs averaging under roughly 100 bytes
(or the equivalent after TCP splits). With page-sized iovecs each MSS spans
at most two frags plus a partial, so the default path is fine. It is
reachable by an unprivileged sender choosing tiny iovecs, so the series
needs an explicit, counted drop for unreadable skbs there (linearize cannot
be the fallback) and a test with small iovecs.

### Answer to "is devmem TX on ionic a driver-only change"

Nothing found in the core at any of the three tags that requires a core
change: the enum exists, bind needs only `netmem_tx` plus a parent device
with a `dma_mask`, all helpers a module needs are inline
(`skb_frag_dma_map`, `skb_frag_is_net_iov`, `netmem_dma_unmap_*`), and no
new exported symbol is involved. The driver work is: set
`netmem_tx = NETMEM_TX_DMA`; skip `dma_unmap_page` for net_iov frags;
handle the 15-frag limit without linearizing; decide policy for SG/csum
being turned off. The unregister gap is a core issue the driver cannot fix,
only avoid in the test procedure.
