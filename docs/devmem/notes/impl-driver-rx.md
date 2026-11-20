# ionic devmem RX: driver series on top of the netmem TX series (v7.4-rc3)

Written 2026-11-18/19. **Built only. Nothing was loaded, booted or run**, so every statement
about behaviour below is from reading source, and the locking in particular has never been
under lockdep. Sections 5 and 6 list what only a boot can confirm and what to exercise first.

Inputs read: `rx-design.md` (Verification pass wins), `rx-interface.md` (normative),
`impl-driver.md`, `testbed.md` section 7, the ionic driver in full where it touches queues
(`ionic_lif.c`, `ionic_txrx.c`, `ionic_ethtool.c`, `ionic_phc.c`, `ionic_bus_pci.c`,
`ionic_dev.c`, `ionic_main.c`, `ionic_aux.c`), and in the core `net/core/netdev_rx_queue.c`,
`netdev_config.c`, `page_pool.c`, `page_pool_user.c`, `devmem.c`, `net/ethtool/rings.c`,
`common.h`, `Documentation/networking/netdevices.rst`, `netmem.rst`. Of the reference drivers
I read only what I needed (bnxt's ops-lock commits by subject, `op_needs_rtnl` users,
`page_pool_is_unreadable()` users, `CHECKSUM_UNNECESSARY` sites); I did not study the
bnxt/gve/fbnic/mlx5 RX datapaths line by line.

## 1. Where things are

| what | where |
| --- | --- |
| Patches (mbox, `git am` format, no Signed-off-by) | `/var/tmp/ionic-devmem/patches-rx/mainline/000[0-7]-*.patch` |
| Tree with all seven applied, uncommitted | `/var/tmp/ionic-devmem/linux-rx` (HEAD still `aebb72744764`) |
| Per-patch file snapshots | `/var/tmp/ionic-devmem/patches-rx/stages/s0` (base = v7.4-rc3 + 6 TX patches) to `s7` |
| Commit messages | `/var/tmp/ionic-devmem/patches-rx/msgs/000[0-7].txt` |
| Scripts | `/var/tmp/ionic-devmem/patches-rx/scripts/`: `mkpatches.sh` (stages + msgs -> mbox), `verify.sh` (apply/compare/mailinfo/checkpatch), `build-series.sh`, `quick.sh`, `snap.sh`, `propagate.sh`, `edit.py` |
| Evidence | `/var/tmp/ionic-devmem/patches-rx/evidence/`: `build.log`, `verify.log`, `verify-codespell.log` |
| Build dirs (mine) | `/var/tmp/ionic-devmem/build-74-rx` (config copied from `build-74`), `/var/tmp/ionic-devmem/build-74-rx-lockdep` (same plus `PROVE_LOCKING`, `DEBUG_NET`, `KASAN`, `INFINIBAND_IONIC=m`). `build-74` was only read. |

To change a patch: edit the tree, `snap.sh N` (or `propagate.sh FILE FROM LAST` when later
patches do not touch the file), `mkpatches.sh`, `verify.sh`, `build-series.sh`.

## 2. Series

| # | subject | files | +/- | needs the invented interface |
| --- | --- | --- | --- | --- |
| 1 | ionic: run the netdev ops under the instance lock | bus_pci, ethtool, lif.c, lif.h, phc | 77/20 | no |
| 2 | ionic: add placeholder definitions for Rx header split | if.h, dev.h | 53/0 | is it |
| 3 | ionic: trust the device's L4 checksum on header split queues | txrx, lif.h, stats, ionic.rst | 41/13 | yes (inert without) |
| 4 | ionic: track Rx buffers as netmem | dev.h, txrx | 57/44 | no |
| 5 | ionic: put headers in the linear area on header split queues | dev.h, lif.c, lif.h, stats, txrx, ionic.rst | 239/57 | yes |
| 6 | ionic: support the tcp-data-split ring parameter | ethtool, lif.c, lif.h | 83/1 | yes |
| 7 | ionic: add queue management ops and allow unreadable Rx payload | lif.c | 162/4 | yes |

Counts are from the diffstat in each patch; the series is 704 insertions, 131 deletions in 10 files. Order differs from `rx-design.md` 5.1 only in that
the checksum patch exists and the pool flags are in the queue-ops patch (a pool with
`PP_FLAG_ALLOW_UNREADABLE_NETMEM` is useless, and untestable, before the queue ops).

## 3. Design decisions, and where they differ from the plan or the spec

1. **Ops lock (patch 1).** `request_ops_lock = true`; the instance lock is the outer lock,
   then `lif->queue_lock`, then what was already nested in `queue_lock`
   (`phc->config_lock`, `dev_cmd_lock`, `vf_op_lock`, `config_lock`). Full path list in
   section 4.
2. **ethtool and RTNL.** In v7.4 an ops-locked driver's ethtool ops run *without* RTNL unless
   it sets `ethtool_ops::op_needs_rtnl` (`netdevices.rst`, "struct ethtool_ops"). I set
   `GLINK` (required: `ethtool_op_get_link()` -> `__linkwatch_sync_dev()`, commit
   1105ef941c1a), `SRINGPARAM` and `SCHANNELS` (cautious; what gve, bnxt, fbnic, mana set).
   Every other ionic ethtool op now runs under the instance lock only. This was not in
   `rx-design.md`.
3. **Checksum (patch 3), narrower than spec D4.** `CHECKSUM_UNNECESSARY` from
   `TCP_OK`/`UDP_OK` only on queues with `IONIC_RXQ_F_HDS`; all other queues keep
   `CHECKSUM_COMPLETE` exactly as today. Reasons: (a) no behaviour change on real hardware,
   whose `_OK` flags I cannot interpret for tunnels; (b) `CHECKSUM_COMPLETE` is the stronger
   report where the payload is readable; (c) with the emulator in mode on and
   `tcp-data-split off`, TCP still goes through `CHECKSUM_COMPLETE`, so K2's byte order keeps
   being tested by the new driver, not only by a stock one. D4 is a SHOULD; recorded in
   `rx-interface.md` under "Changes during implementation".
4. **Unreadable skbs are never handed up without `CHECKSUM_UNNECESSARY`** (spec D4 MUST): they
   are freed and counted (`rx_N_unreadable_drop`, also in `rx_N_dropped`). This happens when
   `ethtool -K rx off` is set, or if the device set `SPLIT` without `TCP_OK`.
5. **Unsplit packet with payload in unreadable SG buffers is dropped** (spec D5), decided from
   `page_pool_is_unreadable(q->page_pool)`, before any buffer is taken.
6. **Header buffer:** 256 bytes (`IONIC_HDS_HDR_BUF_LEN`), a page-pool frag from a second pool
   `q->hdr_pool` that exists only on HDS queues (bnxt's model, not fbnic's always-two-pools).
   The header is copied into `napi_alloc_skb()` and the buffer returned to its pool, as the
   existing copybreak path does; no build_skb on the header buffer.
7. **Payload buffers on an HDS queue are whole pages**, and the SG elements are sized for
   `mtu + VLAN_ETH_HLEN`, not that minus 256 (a split frame leaves `len - 54` in the SG
   elements). Non-HDS queues keep today's frag-allocator sizing.
8. **Driver validates the completion** on an HDS queue before touching buffers:
   `14 <= hds_hdr_len <= min(len, 256)`, `num_sg_elems < nbufs`, every used element has a
   buffer and is needed, and the payload fits the used elements' capacity. Failures count in
   `rx_N_dropped` and leave the buffers attached to the slot (they are re-posted). Not
   validated: that the bytes fit the *posted* element length (the driver, as today, only
   remembers the buffer capacity, which is a page).
9. **Exclusions (patch 6):** HDS with XDP (both directions, `-EOPNOTSUPP`), with Rx timestamps
   on all packets (`IONIC_RXQ_F_HWSTAMP` in `rxq_features`; both directions), with `rx-push`
   (CMB Rx rings). `ETHTOOL_RING_USE_HDS_THRS` is not claimed, so `hds_thresh` stays 0 and
   ncdevmem does not try to set it (it only does if GET reports the attribute).
10. **Capability is checked at run time**, not by `supported_ring_params`:
    `Q_IDENTIFY(RXQ).features` must have `0x20020` and `max_sg_elems != 0`. On a device without
    it GET reports tcp-data-split as unknown and SET `on` returns `-EOPNOTSUPP` with extack.
11. **Queue ops are a stub that deviates from the callback contract** (patch 7):
    `ndo_queue_stop()` writes nothing to its memory; the old rings stay in the lif until
    `ndo_queue_start()` swaps the new ones in, and `start` frees the old ones itself on success
    or swaps back on failure. A `struct ionic_queue_mem` with `qcq == NULL` means "what the lif
    has". I chose this over the contract-faithful version (move the live rings into the
    stop memory) because that version leaves `lif->rxqcqs[i]` without rings between stop and
    start, and for good if both starts fail. It relies on the core only ever passing the stop
    memory to `ndo_queue_start()` (undo) or `ndo_queue_mem_free()`, which is what
    `netdev_rx_queue_reconfig()` does today.
12. **`ionic_qcq_alloc()` gained a `features` argument** (it has to know about HDS to create
    the header pool); the callers' `q.features = ...` assignments after it are gone.

No change to the device interface was needed.

## 4. Ops locking: every path, with the lock state

"core" = the core holds the instance lock for an ops-locked netdev. "takes" = the function now
takes it itself. All citations are to the tree at stage s7.

### 4.1 Functions that now require the instance lock (use `_locked` NAPI helpers)

| function | helper | reached from |
| --- | --- | --- |
| `ionic_qcq_enable()` | `napi_enable_locked` | `ionic_txrx_enable()` <- `ionic_start_queues()`; `ionic_lif_create_hwstamp_{tx,rx}q()` |
| `ionic_qcq_disable()` | `napi_disable_locked` | `ionic_txrx_disable()` <- `ionic_stop_queues()`; error unwind in `ionic_txrx_enable()` |
| `ionic_lif_qcq_deinit()` | `netif_napi_del_locked` | `ionic_txrx_deinit()`, `ionic_txrx_init()` unwind, hwstamp create unwind, `ionic_lif_deinit_locked()`, `ionic_lif_init_locked()` unwind |
| `ionic_lif_txq_init()`, `ionic_lif_rxq_init()` | `netif_napi_add_locked`, `netif_napi_del_locked` | `ionic_txrx_init()`; hwstamp create |
| `ionic_lif_adminq_init()` | `netif_napi_add_locked`, `napi_enable_locked` | `ionic_lif_init_locked()` |
| `ionic_lif_init_locked()` (new name) | `napi_disable_locked` in unwind | `ionic_lif_init()` (takes), `ionic_restart_lif()` (takes) |
| `ionic_lif_deinit_locked()` (new) | `napi_disable_locked`; `netdev_assert_locked()` | `ionic_lif_deinit()` (takes), FW down, `ionic_restart_lif()` unwind, PCI `reset_prepare` |
| `ionic_rxq_pools_create()` with HDS (patch 7) | `page_pool_init()` asserts the lock for `PP_FLAG_ALLOW_UNREADABLE_NETMEM` | `ionic_qcq_alloc()` <- `ionic_txrx_alloc()`, `ionic_reconfigure_queues()`, `ionic_queue_mem_alloc()` |

### 4.2 Entry points

| entry point | who holds the instance lock | RTNL | notes |
| --- | --- | --- | --- |
| `ndo_open` `ionic_open()` | core | yes | `ionic_txrx_alloc/init`, `ionic_start_queues`, `ionic_lif_hwstamp_recreate_queues`; `netif_set_real_num_{tx,rx}_queues` assert the instance lock only |
| `ndo_stop` `ionic_stop()` | core | yes | does not cancel work, so no wait on a work item that wants the lock |
| `ndo_change_mtu` | core (`dev_set_mtu` -> `netdev_lock_ops`) | yes | stop/start reconfig; start is deferred to the link-check work, which then takes the lock |
| `ndo_bpf` `ionic_xdp_config()` | core (`dev_xdp_install` asserts) | yes | `ionic_reconfigure_queues()` |
| `ndo_hwtstamp_set` `ionic_hwstamp_set()` | core (`dev_set_hwtstamp`, tsconfig) | yes | creates hwstamp queues (NAPI add) or reconfigures all Rx queues |
| `ndo_tx_timeout` | no (atomic) | no | only schedules `tx_timeout_work` |
| `ionic_tx_timeout_work()` | **takes**, then `queue_lock`; `netif_running()` now checked under the lock | no | |
| `ndo_set_rx_mode`, `ndo_set_features`, `ndo_set_mac_address`, `ndo_vlan_rx_*`, `ndo_set_vf_*`, `ndo_get_stats64` | whatever the core does | | adminq commands only; no NAPI, no queue changes; unchanged |
| ethtool `set_ringparam`, `set_channels` | core | yes (`op_needs_rtnl`) | `ionic_reconfigure_queues()` / `ionic_cmb_reconfig()` |
| ethtool `get_link` | core | yes (`op_needs_rtnl`) | `ethtool_op_get_link()` |
| every other ethtool op | core | **no longer** | device commands and driver state only; I found no core helper in them that asserts RTNL |
| deferred work `IONIC_DW_TYPE_LINK_STATUS` -> `ionic_link_status_check()` | **takes** around both the start and the stop branch, then `queue_lock` | no | carrier on/off and the adminq-free parts stay outside |
| `ionic_link_status_check_request(CAN_SLEEP)` from `ionic_lif_register()` and the end of `ionic_restart_lif()` | same function, **takes** | no | both callers have dropped all locks |
| deferred work `LIF_RESET` down -> `ionic_lif_handle_fw_down()` | **takes** after `ionic_auxbus_unregister()`, then `queue_lock`, across stop/deinit/free and `ionic_lif_deinit_locked()` | no | `IONIC_LIF_F_FW_RESET` is still set before any lock |
| deferred work `LIF_RESET` up -> `ionic_restart_lif()` | **takes**, then `queue_lock`, across `ionic_lif_init_locked()` and `ionic_txrx_alloc/init`; both released before the link check, `netif_device_attach()`, `ionic_auxbus_register()` | no | |
| `ionic_lif_hwstamp_replay()` (after FW up) | **takes**, then `queue_lock` | no | |
| PCI `reset_prepare` (also `error_detected`) | **takes** after cancelling the deferred work and unregistering the aux device | no | under `device_lock` |
| PCI `reset_done` -> `ionic_restart_lif()` | as above | no | |
| probe: `ionic_lif_init()` | **takes** (netdev not registered yet) | no | adminq NAPI add/enable |
| probe unwind and remove: `ionic_lif_deinit()` | cancels `deferred.work` and `tx_timeout_work` first, **then takes** | no | in remove the netdev is already unregistered; its lock exists until `free_netdev()` |
| deferred work `RX_MODE` | not taken | no | adminq only |
| `ionic_doorbell_check_dwork()` | not taken; `queue_lock` only | no | only schedules NAPI; `queue_lock` never has the instance lock taken inside it |
| netdev notifier `NETDEV_CHANGENAME` | core (documented as running under the instance lock) | yes | adminq only |
| queue ops (patch 7) | core | **no** | see 5.2 |

### 4.3 Things that must stay outside the instance lock

- `cancel_work_sync(&lif->deferred.work)` and `(&lif->tx_timeout_work)`: both works take the
  lock. Sites: `ionic_lif_deinit()` cancels both (before its lock); `ionic_reset_prepare()`
  cancels only `deferred.work`; `ionic_remove()` cancels neither itself (it reaches
  `ionic_lif_deinit()`). (Corrected in the review round: this list used to name
  `ionic_remove()` and `ionic_reset_prepare()` as cancelling `tx_timeout_work`.) So the Tx
  timeout work can be waiting for the instance lock while a FW or PCI reset frees the queues;
  it now tests `netif_running()` and `IONIC_LIF_F_FW_RESET` after taking the lock.
  `ionic_lif_deinit_locked()` does not cancel; its callers are all in
  `IONIC_LIF_F_FW_RESET`, where the old code skipped the cancel too.
- `destroy_workqueue(ionic->wq)` in `ionic_dev_teardown()`: after unlock in `reset_prepare`.
- `ionic_auxbus_register()` / `ionic_auxbus_unregister()`: the RDMA driver's probe/remove run
  inside them, and the RDMA core takes RTNL and, through `ib_get_eth_speed()` ->
  `__ethtool_get_link_ksettings()` -> `netdev_lock_ops()`, **this netdev's instance lock**.
  They were already outside `queue_lock` and are outside the instance lock in all four callers.
- `drivers/infiniband/hw/ionic` itself calls no NAPI or netdev-lock API (grep), and nothing
  exported to it changed.

## 5. What only a boot can confirm, most risky first

1. **Patch 1 deadlocks or lockdep splats.** Candidates I could not rule out by reading:
   - RDMA: an IB cache update or port query (`ib_get_eth_speed`) racing a path that holds the
     instance lock for long (FW recovery, reconfigure). It should only wait, but the RDMA
     driver was not built into any lockdep run, and the stack loads it on this netdev.
   - `instance lock -> queue_lock -> page_pools_lock` (page_pool create/destroy) against
     netdev-genl page-pool dumps; same pattern as other ops-locked drivers, unverified.
   - `ionic_lif_init()` now holds the instance lock of an unregistered netdev across
     `LIF_INIT`, feature setup and `ionic_station_set()`; I found nothing in there that takes
     it again, by reading only.
   - A deferred link check now waits for the instance lock while behind it in the same work
     item a `LIF_RESET` may be queued; an ndo stuck in an adminq wait then ends by timeout
     rather than by `FW_RESET`. Same shape as before with `queue_lock`, but worth a FW-down
     test under load.
2. **Ethtool ops without RTNL** (all but ring/channels/get_link). If one of them needs RTNL
   through something I missed, lockdep's `ASSERT_RTNL` says so the first time it runs:
   `ethtool -C`, `-A`, `-s`, `-X`, `--set-fec`, `-r`, `-m`, tunables.
3. **Queue ops without RTNL (patch 7).** `ionic_stop_queues_reconfig()`, `ionic_txrx_init()`,
   `ionic_start_queues()` have only ever run under RTNL. I found no RTNL-asserting helper in
   them (`netif_set_real_num_*` is not called by the stub).
4. **The stub's contract deviation** (decision 11): correct only as long as the core uses the
   stop memory as it does today. Failure paths (start fails, undo start) are untested; they
   need fault injection or a hacked emulator.
5. **Pool pick-up on bind:** that the shadow queue's payload pool, created in
   `ndo_queue_mem_alloc()` with `PP_FLAG_ALLOW_UNREADABLE_NETMEM` and `queue_idx`, is the one
   `page_pool_check_memory_provider()` finds (else bind fails `-ENODATA`), and that after an
   ifdown/ifup or a ring resize the new pools pick the binding up again.
6. **HDS skb shape through GRO and TCP:** header-only linear area + net_iov frags from
   `napi_gro_receive()`; `skb->unreadable` set; `tcp_recvmsg_dmabuf()` sees no linear payload
   for split packets.
7. **Device/driver agreement** on the 32-byte completion (ext at offset 0, base at 16), on
   `hds_hdr_len` semantics, and on `Q_INIT.features = 0x20020` with `IONIC_QINIT_F_SG`. The
   driver also sends `0x20020` for *every* Rx queue when HDS is on; the hwstamp Rx queue
   (index `nrxqs_per_lif`) stays `0x10020`, which the emulator would refuse, but it never
   offers timestamps.
8. **Checksum:** `CHECKSUM_UNNECESSARY` on HDS queues; `CHECKSUM_COMPLETE` byte order (K2) on
   non-HDS queues with mode on ("hw csum failure" in dmesg if wrong).
9. **Header pool sizing and recycling:** 16 header buffers per 4K page as page-pool frags,
   returned with `page_pool_put_full_netmem(..., true)` from NAPI; watch for page-pool leak
   warnings at ifdown ("stalled pool shutdown").
10. **DMA to dmabuf addresses:** `desc`/SG addresses come from
    `page_pool_get_dma_addr_netmem()`; masked nowhere on Rx. That the emulator can write there
    is the emulator's and the stack's matter.
11. **Whole-page payload buffers** make an HDS queue use more memory per posted descriptor
    than a non-HDS one (one page per descriptor at MTU 1500, plus the header frag); with 4096
    descriptors and a small udmabuf the pool can run dry. `rx_N_alloc_err` does not count
    failed buffer allocations in `ionic_rx_fill()` (it never did); a starved queue just stops
    receiving until buffers return.

## 6. Checklist for the integration engineer, in this order

Use KASAN + lockdep (`PROVE_LOCKING`), the RDMA driver loaded, and read `dmesg` after every
step; `ethtool -S` counters named below are per queue.

1. Patch 1 alone (apply only 0001) against the *unmodified* emulator: load, `ip link set up`,
   ping, `ip link set down`, repeat a few times; `ethtool -G rx 512`, `ethtool -L combined 1`
   and back, `ip link set mtu 9000` and back, attach/detach an XDP program, `ethtool -C`,
   `ethtool -A`, `ethtool -X equal 2`, `ethtool <if>` (get_link), `rmmod ionic_rdma; rmmod
   ionic` while up. If the emulator can drop link or simulate FW down/up (heartbeat stop),
   do both while traffic runs, and a PCI function reset
   (`echo 1 > /sys/bus/pci/devices/.../reset`) while up and while down.
2. Whole series, emulator mode **off**: everything in 1 again, plus `ethtool -g` must show
   tcp-data-split as n/a (the attribute is not reported), and `ethtool -G <if> tcp-data-split on` must fail with "Device does not
   support tcp-data-split". Plain TCP/UDP must count `rx_N_csum_none` as before.
3. Mode **on**, tcp-data-split off: plain TCP and UDP, no "hw csum failure";
   `rx_N_csum_complete` rises, `rx_N_csum_unnecessary` stays 0.
4. `ethtool -G <if> tcp-data-split on` (queues restart): `ethtool -g` shows it on; iperf/nc
   with payload hashes both directions; `rx_N_hds_split` and `rx_N_csum_unnecessary` rise;
   `rx_N_dropped`, `rx_N_unreadable_drop` stay 0. Then: MTU 9000 (multi-element payload),
   ping -s 2000 (unsplit, readable, must work), UDP, IPv6 TCP, VLAN if available. Then
   `ethtool -K <if> rx off` (still works with readable buffers, via software checksum).
   Exclusions: XDP attach must fail; `tcp-data-split on` with XDP attached must fail.
   ifdown/ifup with HDS on; ring resize with HDS on; `tcp-data-split off` again.
5. Bind (needs the netdev **up**): `ethtool -L <if> combined 1`, HDS on,
   `ncdevmem -n -l -s <ip> -p 5201 -f <if> -t 0 -q 1 -v 7 -L`, sender with TCP timestamps on
   (without them a 1..5 byte payload is padded, not split, and `-L` fails by design).
   Expect a full queue restart at bind and at unbind (link stays up, a short traffic gap).
   Verify payload at the receiver. Then with the binding in place: ifdown/ifup (binding must
   survive), `ethtool -G rx 1024`, `tcp-data-split off` (core must refuse), XDP attach (core
   must refuse), `ethtool -L` shrink below the bound queue (core must refuse),
   `ethtool -K rx off` (devmem payload packets are then dropped, `rx_N_unreadable_drop`
   rises: expected), a large non-TCP packet to the bound queue (`ping -s 2000`: dropped,
   `rx_N_unreadable_drop`: expected), kill ncdevmem (unbind), and bind while down (must fail).
6. Two queues with the sender's source port chosen per `rx-interface.md` T3; bind queue 1;
   check that queue 0 traffic survives the restart.
7. `rmmod` with a binding active; FW down/up with a binding active if the emulator can.

## 7. What was run

Host x86_64, gcc 13.3.0, bison from `tools/bison`, sparse from `tools/sparse-src`.

| check | result | evidence |
| --- | --- | --- |
| Stage s0 equals v7.4-rc3 blobs + the six TX patches (`git apply`) | PASS, 28 files | `evidence/verify.log` |
| Each RX patch: `git apply --check` and `git apply --whitespace=error-all` in order, result byte-identical to stage sN | PASS, 7/7 | `evidence/verify.log` |
| `linux-rx` working tree equals the whole series | PASS | `evidence/verify.log` |
| `git mailinfo` on all 8 files (author Stephen Bates <sbates@raithlin.com>) | PASS | `evidence/verify.log` |
| `checkpatch.pl --strict`, also `--codespell` | PASS: cover 0/0/0; each patch 1 error = "Missing Signed-off-by" only, 0 warnings, 0 checks | `evidence/verify.log`, `evidence/verify-codespell.log` |
| W=1 build of `drivers/net/ethernet/pensando/ionic/` at s0..s7, `build-74-rx` | PASS, 13 objects each, 0 warnings | `evidence/build.log`, `build-74-rx/series-w1-s*.log` |
| sparse `C=2 W=1` at s0..s7, `build-74-rx` | PASS, 13 files checked each, 0 warnings | `evidence/build.log`, `build-74-rx/series-sparse-s*.log` |
| W=1 at s0..s7 with `PROVE_LOCKING`, `DEBUG_NET`, `KASAN` | PASS, 0 warnings | `evidence/build.log`, `build-74-rx-lockdep/series-lockdep-s*.log` |
| RDMA driver `drivers/infiniband/hw/ionic/` builds against s7 | PASS, 8 objects, 0 warnings | `build-74-rx-lockdep/rdma-s7.log` |
| `git am` | NOT RUN (it commits) | |
| modpost / `ionic.ko` link, full kernel build, clang, other architectures | NOT RUN | |
| Any load, boot, bind, traffic, lockdep, KASAN, selftest | NOT RUN | |
| Backport to the v7.2.6 DKMS sources / build against v7.2.15 | NOT RUN (not asked; patch 1 uses `op_needs_rtnl` and patch 7 the v7.4 queue-ops signatures, so it will not apply as is) | |
| `make htmldocs` for `ionic.rst` (6 counter lines added) | NOT RUN | |

Only objects were built (`make O=... W=1 drivers/net/ethernet/pensando/ionic/`).

## 8. Seen in ionic and left alone

- `ionic_lif_rxq_init()` adds a NAPI for the hwstamp Rx queue, which has no
  `IONIC_QCQ_F_INTR`; `ionic_lif_qcq_deinit()` only deletes NAPIs of queues with that flag, so
  that NAPI appears never to be deleted before its qcq is freed. Not reachable on the emulator
  (no timestamp feature). Not verified by running.
- `ionic_run_xdp()` syncs and fills XDP frags with `buf_info->page_offset` (the first
  buffer's) and offset 0 instead of each buffer's own offset. Kept as is in the netmem
  conversion.
- `struct ionic_buf_info::dma_addr` is unused on Rx since the TX series gave Tx its own struct.
- `ionic_stop()` returns early in `IONIC_LIF_F_FW_RESET`; if FW-down handling runs after
  `__LINK_STATE_START` was cleared it skips `ionic_txrx_free()`. Pre-existing ordering,
  timing slightly changed by the new outer lock.
- Rx buffer allocation failures in `ionic_rx_fill()` are not counted anywhere.

## 9. Not done

- No n-tuple steering, RSS contexts or `netif_queue_set_napi()`; `queue-get` will not show a
  NAPI id for ionic queues.
- No real single-queue restart; no `ndo_validate_qcfg`/`ndo_default_qcfg`; the queue config
  passed to the callbacks is ignored (ring size comes from the lif).
- No documentation beyond the counter list in `ionic.rst`.

## Review round: integration run (2026-11-18, `test-rx.md`)

Supersedes "Nothing was loaded, booted or run" above. The series was run in stack 2 (guests on
7.4.0-rc3 KASAN + lockdep, `ionic_rdma` loaded, emulator in header-split mode and with it off).

- **One bug found and fixed, in patch 1:** `register_netdevice()` refuses an ops-locked netdev
  that still has `ndo_set_rx_mode` (`netdev_check_ops()`: "ops-locked drivers should use
  ndo_set_rx_mode_async", `-EINVAL`), so the module did not probe. `ionic_ndo_set_rx_mode()`
  is now `ndo_set_rx_mode_async`: it syncs the address-list snapshots with
  `__hw_addr_sync_dev()` and calls `ionic_lif_rx_mode()` directly (process context, instance
  lock held) instead of queueing `IONIC_DW_TYPE_RX_MODE`. Stages s1..s7, `msgs/0001.txt`,
  `msgs/0000.txt` (testing paragraph) and `mainline/` regenerated; `verify.sh` and W=1 at
  s0..s7, sparse at s1 and s7 pass (`patches-rx/evidence/verify-testrx.log`,
  `build-testrx.log`). Section 4.2's row "`ndo_set_rx_mode` ... unchanged" is therefore wrong
  now: it runs under the instance lock and takes `config_lock` inside it.
- Section 5 risks 1 to 3, 5, 6, 7, 8, 9: exercised, nothing reported by lockdep or KASAN.
  Risk 4 (stub failure paths) and FW down/up through the heartbeat: not run.
- Section 6, "senders need TCP timestamps": not true on this stack (frames are not padded).
- Pre-existing bugs seen (not from the series): the temporary-queue leak in
  `ionic_reconfigure_queues()` (fix offered separately in
  `patches-rx/extra/0001-ionic-free-every-temporary-queue-after-a-queue-count-change.patch`,
  not applied to the tree), and no `dma_set_max_seg_size()` (DMA-API warning at every bind).

## Review round 2 (2026-11-19, independent review; details and evidence in `test-rx.md` "Review round")

Supersedes the series table in section 2 and decision 11 where they differ.

- **The series is 8 patches** (`patches-rx/mainline/000[0-8]`, stages `s0..s8`, `linux-rx` =
  `s8`; `mkpatches.sh` has `N=8` and emits `prerequisite-patch-id` for the six TX patches).
  New patch 7, "ionic: keep an Rx queue going when its buffers run out": `ionic_rx_fill()`
  rings the doorbell for what it posted when an allocation fails, counts `rx_N_alloc_err`,
  and arms `qcq->rx_refill_timer` (10 ms, `napi_schedule()`) only when the ring is empty;
  `ionic_qcq_enable()` re-arms it if the first fill got nothing; it is deleted in
  `ionic_qcq_disable()` and `ionic_qcq_free()`. Section 5 item 11 ("just stops receiving until
  buffers return") was wrong: it never came back.
- Patch 1: `ionic_tx_timeout_work()` tests `netif_running()` and `IONIC_LIF_F_FW_RESET` under
  the instance lock.
- Patch 6: any explicit tcp-data-split setting is refused on a device without the feature.
- Patch 8 (queue ops): `ionic_queue_start()` attaches the netdev on failure too; with
  `IONIC_LIF_F_FW_RESET` set it leaves the new rings in the lif, frees the old ones and returns
  0 (the reset, waiting for the lock, frees everything and recovery rebuilds from the queue's
  provider); `ionic_restart_lif()` clears `IONIC_LIF_F_FW_RESET` before dropping the instance
  lock; `ionic_swap_queues()` sets `p.napi = NULL` on the pools that go to the temporary queue
  (also for `ionic_reconfigure_queues()`); the header pool comment is corrected.
- Messages: patch 2 and the cover letter say `BIT_ULL(17)` is unchecked against firmware;
  patch 3's fbnic sentence; patch 5's larger receive size; cover letter testing rewritten.
- Section 4.3 corrected in place.
- Test-only knobs (`patches-rx/extra/test-only-ionic-fault-knobs.diff`, not in the series):
  `fi_qstart_fail`, `fi_qop_fw_reset`, `fi_link_down`, `fi_rx_alloc_fail`, `fi_cmd`
  (`fwdown`, `fwdown-hold`, `fwup`, `txtimeout`, `linkcheck`) under
  `/sys/module/ionic/parameters/`.
- Still open: a real single-queue restart; a firmware that really dies during recovery; after
  a double start failure the watchdog's link check can call `ionic_start_queues()` on queues
  that were not re-initialised (same as after a failed `ionic_start_queues_reconfig()`
  upstream; nothing bad was seen in `R11`, the queues simply stayed down until ifdown/ifup).

## XDP frag offset fix (2026-11-19, `rx-xdp-frag-fix.md`)

Supersedes section 8's second item ("Kept as is in the netmem conversion") and the base in
section 1. Stage `s0` is now v7.4-rc3 + `patches/xdp-frag-offset/net/0001-*` + the six TX
patches; `mkpatches.sh` lists the fix's patch-id first among the prerequisites and `verify.sh`
builds the base that way. Patch 4 converts the fixed lines, so `ionic_run_xdp()` uses
`bi->page_offset` for the sync and the frag fill at every stage. `verify.sh`, W=1 and sparse
at s0..s8 pass (`patches-rx/evidence/verify-xdpfix.log`, `build-xdpfix.log`). Run as module
`rx6`.
