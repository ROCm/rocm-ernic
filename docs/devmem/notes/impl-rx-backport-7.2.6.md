# ionic devmem RX: backport to the ROCm 10.2 guest sources (v7.2.6 + srq + p4 + devmem-tx)

Written 2026-11-19 (night, host time). **Compile-only: nothing was loaded, booted or run; no
stack was touched.** Nothing was committed, pushed or posted; `rocm-ernic` was only read
(`git archive 2d3a3d02`). Everything is under `/var/tmp/ionic-devmem/overnight2/rx-backport/`
(`O/` below).

## 1. Result

The eight RX patches (`patches-rx/mainline/0001..0008`, as regenerated on the XDP frag-offset
fix) are backported to the tree the ROCm 10.2 guests build: v7.2.6's three ionic directories
with `patches/srq` 0001-0011, `patches/p4` and `patches/devmem-tx` (rocm-ernic 2d3a3d02). That
base is byte-identical to `test-rocm102/ionic-mod/devmem/tree`, the tree whose modules stack 3
runs now (`diff -rq`).

**No code had to change.** The end state is the v7.4-rc3 series' end state plus exactly the
lines in which v7.2.6+srq+p4 already differed from v7.4-rc3 (proved per file and per stage,
section 6). Seven of the eight diffs are the mainline diffs but for line numbers and the two
`Documentation/` hunks (not in the sparse checkout); patch 1 differs in one context line
(`ionic_lif_hwstamp_replay()` has no `hw_features` test in v7.2.6). What differs is what the
core does around the driver, and it is written into the patch notes and section 3.

| What | Where |
| --- | --- |
| Patches (git-am mbox, `From: Stephen Bates <sbates@raithlin.com>`, `[PATCH]` subjects, a `[ rocm-ernic: backport of RFC patch n/8 ...; differences: ... ]` paragraph each, **no Signed-off-by**) | `O/patches/0001..0008-*.patch`, sha256 in `O/evidence/patches.sha256` |
| Full source tree, kernel-root layout (v7.2.6 + srq + p4 + devmem-tx + rx) | `O/tree/` |
| The same in `ionic-mod.sh`'s layout (`tree/`, `mod/`, `stage.tgz`, `SOURCE`) | `O/guest/` |
| Base, stages `b0..b8`, per-patch build dirs | `O/base/`, `O/stages/`, `O/build/<base,b1..b8,tree>/{src,mod,p4}` |
| Scripts | `O/scripts/`: `regen.sh` (everything, in order), `mkbase.sh`, `mkstages.sh`, `mkpatches.sh`, `verify.sh`, `combos.sh`, `stage-builds.sh`, `build.sh` + `build-in-container.sh`, `build-generic.sh` + `build-generic-in-container.sh`, `env.sh` |
| Inputs kept with it | `O/src/ernic/` (the `git archive`: `ansible/files/patches`, `ansible/files/scripts`, `drivers/ernic_p4`), `O/notes/N.txt` (the backport paragraphs) |
| Evidence | `O/evidence/` |

`regen.sh` rebuilds all of it from the inputs (old outputs go to `O/old/`, never deleted).
It rewrites the `Date:` lines, so the patch sha256 sums change on every run.

## 2. What differs between v7.2.6 and v7.4-rc3

### ionic (`git diff v7.2.6 v7.4-rc3 -- drivers/net/ethernet/pensando/ionic`, and base vs mainline stage `s0`)

Files the RX patches touch, with what the base differs in from the mainline base
(`O/evidence/mkstages.log`, lines differing):

| File | Base vs `s0` | What | Matters to the series |
| --- | --- | --- | --- |
| `ionic_txrx.c`, `ionic_dev.h`, `ionic_stats.c`, `ionic_txrx.h` | 0 | identical (devmem-tx 0001 brought `ionic_txrx.c` to v7.2.15 = v7.4-rc3; srq/0010 is the XDP frag-offset fix the series sits on) | no |
| `ionic_lif.c` | 38 | `kzalloc_obj` conversions, `IONIC_ETH_HW_RDMA_TIMESTAMP`, PHC state page (v7.4 only); the P1 leak fix of srq/0011 (base only) | no: hunks apply at offsets -3/-10 |
| `ionic_lif.h` | 4 | `p4_adev` (p4), PHC state page | no |
| `ionic_if.h` | 3 | `IONIC_LIF_CAP_P4` (p4), `IONIC_ETH_HW_RDMA_TIMESTAMP` | no: different enum than `IONIC_RXQ_F_HDS` |
| `ionic_ethtool.c` | 22 | `assign_bit`, ts_info | no |
| `ionic_phc.c` | 65 | PHC state page; `ionic_lif_hwstamp_replay()` without the `hw_features` test | context of one hunk (fuzz 1) |
| `ionic_bus_pci.c` | 2 | v7.4 adds `ionic_port_reset()`/`ionic_vf_dealloc()` on two paths | no |

Not touched by the series but different: `ionic_aux.c` (p4's second aux device),
`ionic_debugfs.c`, `ionic_main.c`, `ionic_rx_filter.c`.

### Core interfaces the series uses, read in `/var/tmp/ionic-devmem/linux-7.2.6`

| Interface | v7.2.6 | Verdict |
| --- | --- | --- |
| `netdev_queue_mgmt_ops`, callback signatures (`ndo_queue_mem_alloc(dev, qcfg, mem, idx)`, `ndo_queue_start(dev, qcfg, mem, idx)`, `ndo_queue_stop(dev, mem, idx)`, `ndo_queue_mem_free(dev, mem)`) | same (`netdev_queues.h` differs only by the ring-size fields v7.4 added to `netdev_config`/`netdev_queue_config`, which the stub ignores) | same |
| `netdev_rx_queue_restart()`/`netdev_rx_queue_reconfig()`, `__netif_mp_open_rxq()` (hds check, undo with the stop memory) | `net/core/netdev_rx_queue.c` is **identical** in both tags | same; decision 11's contract deviation holds as on v7.4 |
| bind/unbind: `netdev_nl_bind_rx_doit()`, `netdev_nl_sock_priv_destroy()` | instance lock, no RTNL | same |
| page pool: `PP_FLAG_ALLOW_UNREADABLE_NETMEM` + `queue_idx`, `netdev_assert_locked()` in `page_pool_init()`, `page_pool_is_unreadable()`, netmem helpers | same; `mp_dmabuf_devmem_init()` wants order 0, `dma_map`, forces `dma_sync` off | same |
| devmem allocator | gen_pool, one net_iov per allocation; SG lengths must be `PAGE_SIZE` aligned. v7.4: freelist, 64 at a time, niov size option | core only; slower refill, same driver contract |
| dma-buf attach | **static** (`dma_buf_attach()`, `net/core/devmem.c:229`); v7.4 commit 69ab87acc614 makes it dynamic + p2p. `7.2.6-devmemp2p` has that one commit backported | core only; decides where a GPU buffer lives, see section 7 |
| ethtool: `ETHTOOL_RING_USE_TCP_DATA_SPLIT`, `kernel_ringparam.tcp_data_split`, `dev->cfg->hds_config`/`hds_thresh`, `cfg_pending` | same (v7.4 adds ring sizes to `cfg` and a revalidate hook) | same |
| ethtool locking: `op_needs_rtnl`, `ETHTOOL_OP_NEEDS_RTNL_{GLINK,SRINGPARAM,SCHANNELS}`, ioctl and netlink paths | **present and the same**: an ops locked driver's ethtool ops run under the instance lock and without RTNL unless the bit is set (v7.4 only adds `_TEST`) | same |
| `request_ops_lock`, `netdev_need_ops_lock()`, `netdev_lock_ops()` in `dev_api.c`, `dev_ioctl.c`, `link_watch.c` | `dev_api.c` and `link_watch.c` identical; `netdev_lock.h` differs in lockdep classes only | same |
| `_locked` NAPI helpers, `netif_set_real_num_{tx,rx}_queues()` (`netdev_assert_locked_ops_compat`) | same (v7.4 adds a `NAPI_STATE_DISABLED` bit internally) | same |
| `ndo_set_rx_mode_async` | present, same signature, same `netif_rx_mode_run()` | **two differences**, next rows |
| ops locked netdev that keeps `ndo_set_rx_mode` | `register_netdevice()` only **warns** (`netdev_WARN`, `net/core/dev.c:11389`); v7.4 returns `-EINVAL` (05fe748456ea) | conversion kept: no warning, same code as mainline |
| who holds what when the async op runs from the work | `netdev_work_proc()` takes **`rtnl_lock()` and then the instance lock**; v7.4 takes the instance lock only (5a81c72ef9ad) | driver code is fine with both; see 3.2 |
| notifiers | `NETDEV_CHANGE`, `CHANGENAME`, `REGISTER`, `UP` under the instance lock; v7.4 adds `DOWN`/`GOING_DOWN` (5326fefb9fe8, f540caaaca8d) | ionic's notifier only handles `CHANGENAME`; ionic_rdma has none |
| `ib_get_eth_speed()` | `rtnl_lock()` + `__ethtool_get_link_ksettings()` -> `netdev_lock_ops()`; v7.4 also tests `reg_state` first (ef9fbe1b93f3) | see section 4, A1 |
| RDMA core: `device.c`, `roce_gid_mgmt.c`, `cache.c` | netdev events are handled from work items in both; `device.c` differs in naming/netns code only | same |

Kernel fixes in v7.4-rc3 that touch what devmem RX uses and are **not** in v7.2.6 (checked by
subject in `v7.1..v7.2.6`): the `page_pool_recycle_ring_bulk()` use-after-free (in v7.2.7+),
"tcp: reject net_iov in zerocopy receive mapping hints" (v7.2.7+), "propagate unreadable flag
in skb_zerocopy", "page_pool: keep frag_offset aligned" (v7.2.7+; harmless here, the header
pool only hands out 256-byte pieces and ionic builds no skb head on a frag), and the p2p attach
above. v7.2.6 does have the `__page_pool_release_netmem_dma` UAF fix and the
`skb_copy_ubufs()` readable check.

## 3. Ops locking on v7.2.6: every path, re-derived

"core" = v7.2.6's core holds the instance lock (cited). "takes" = ionic takes it. Lock order,
as on v7.4: RTNL (when held) -> instance lock -> `lif->queue_lock` -> `phc->config_lock`,
`dev_cmd_lock`, `vf_op_lock`, `lif->config_lock`, `page_pools_lock`.

### 3.1 Entry points

| Entry point | Instance lock | RTNL on v7.2.6 | v7.4-rc3 | Notes |
| --- | --- | --- | --- | --- |
| `ndo_open` `ionic_open()` | core (`dev_open()`, `do_setlink()`; `__dev_open()` asserts RTNL) | yes | same | `netif_set_real_num_*` want the ops lock only |
| `ndo_stop` `ionic_stop()` | core (`dev_close()`; unregister locks then `netif_close_many()`) | yes | same | cancels no work |
| `ndo_change_mtu` | core (`dev_set_mtu()`) | yes | same | |
| `ndo_bpf` `ionic_xdp_config()` | core (`dev_xdp_install()` asserts) | yes | same | |
| `ndo_hwtstamp_set/get` | core (`dev_ioctl.c`; tsconfig netlink is in `ethtool_nl_msg_needs_rtnl()`) | yes | same | not reachable on the emulator (no PHC) |
| `ndo_set_mac_address`, `ndo_set_features`, `ndo_set_vf_*` | core | yes | same | adminq only |
| `ndo_vlan_rx_add/kill_vid` | whatever the caller has (`vlan_core.c` asserts RTNL only) | yes | same | adminq only |
| `ndo_get_stats64`, `ndo_start_xmit`, `ndo_xdp_xmit`, `ndo_tx_timeout` | no | no | same | |
| **`ndo_set_rx_mode_async`** from `netdev_work_proc()` | core | **yes** | **no** | the one lock-state difference. `ionic_lif_rx_mode()` takes `config_lock` and waits for adminq commands with RTNL held |
| `ndo_set_rx_mode_async` from `netif_rx_mode_sync()` (flags, promisc, allmulti changes) | core | yes | yes | |
| ethtool `set_ringparam`, `set_channels`, `get_link` | core | yes (`op_needs_rtnl`) | same | `ionic_reconfigure_queues()` calls `netif_set_real_num_tx_queues()` only from here, XDP and hwstamp, all with RTNL |
| every other ethtool op (`-c/-C`, `-a/-A`, `-s`, `-x/-X`, `-k/-K` aside, FEC, `-r`, `-m`, `-S`, `-d`, tunables) | core | **no** (was yes before patch 1) | same | device commands and driver state only |
| queue ops (patch 8): bind, unbind, socket close | core (`netdev_get_by_index_lock()`, `netdev_lock()`) | **no** | same | `ionic_stop_queues_reconfig()`, `ionic_txrx_init()`, `ionic_start_queues()` without RTNL |
| notifier `NETDEV_CHANGENAME` (the netdev is renamed to `rocm-ernic0` in these guests) | core | yes | same | adminq only, takes nothing |
| deferred work `LINK_STATUS` -> `ionic_link_status_check()` | **takes**, then `queue_lock` | no | same | also from `ionic_lif_register()` and the end of `ionic_restart_lif()`, both with no lock held |
| deferred work `RX_MODE` (now only from the watchdog's filter retry, `ionic_dev.c:44`) | no | no | same | `config_lock` serialises it against the async op |
| deferred work `LIF_RESET` down -> `ionic_lif_handle_fw_down()` | **takes** after `ionic_auxbus_unregister()` | no | same | |
| `LIF_RESET` up, PCI `reset_done` -> `ionic_restart_lif()` | **takes**; `ionic_auxbus_register()` after both unlocks | no | same | |
| `ionic_lif_hwstamp_replay()` | **takes** (after the `!lif->phc` return, which is what the emulator hits) | no | same | |
| `ionic_tx_timeout_work()` | **takes**, tests `netif_running()` and `FW_RESET` under it | no | same | |
| PCI `reset_prepare` / `error_detected` | **takes** after `cancel_work_sync(deferred)` and `ionic_auxbus_unregister()` | no | same | under `device_lock` |
| probe `ionic_lif_init()`; probe unwind and remove `ionic_lif_deinit()` | **takes** (netdev unregistered; in remove after both works are cancelled) | no | same | |
| `ionic_doorbell_check_dwork()`, `dim.work`, `doorbell_napi_work`, `rx_refill_timer` | no | no | same | `queue_lock` or nothing; cancelled under the instance lock, take none |

### 3.2 What the v7.2.6 difference means

- `netdev_work_proc()` holding RTNL while it waits for the instance lock adds no new order
  (RTNL -> instance is the core's order everywhere). It does mean that an rx_mode sync
  (multicast join, `tcpdump`'s promiscuous toggle, `ip link set ... allmulti`) now does its
  adminq commands **with RTNL held**; before patch 1 they ran from ionic's own work with no
  lock but `config_lock`. With a firmware that stops answering, each waits for its adminq
  timeout under RTNL. On v7.4 the work path does not hold RTNL. Not a deadlock; a longer RTNL
  hold when the device is sick.
- Because v7.2.6 only warns, the driver would also register with `ndo_set_rx_mode` kept. It
  is converted anyway: the warning is a `WARN` splat with a taint at every probe.
- Nothing in ionic takes RTNL while holding the instance lock (grep of the tree: no
  `rtnl_lock`, `dev_open`, `dev_close`, `netdev_update_features` anywhere in the driver).

### 3.3 Must stay outside the instance lock (unchanged from v7.4, re-checked in this tree)

`cancel_work_sync()` of `deferred.work` and `tx_timeout_work` (`ionic_lif_deinit()`,
`ionic_reset_prepare()`, `ionic_remove()`); `destroy_workqueue()`;
`ionic_auxbus_register()`/`_unregister()` at all five call sites (`ionic_bus_pci.c:381`,
`:424`, `:454`, `ionic_lif.c:3665`, `:3737` in `O/tree`).

## 4. The RDMA aux driver and the P4 module under ops locking

`ionic_rdma` (v7.2.6 + srq 0001-0009) and `ernic_p4` (rocm-ernic `drivers/ernic_p4`, 0.2.0;
the source the image bakes: my builds have the srcversions the baseline note lists,
`24E57766...` and `66D2E219...`) were built against the result after every patch. Neither
calls a NAPI, netdev-lock or RTNL API (grep). What they use of ionic: `ionic_adminq_post_wait`,
`ionic_error_to_errno`, `ionic_request_rdma_reset`, `ionic_intr_alloc/free`,
`ionic_get/put_cmb` (ionic_rdma); `ionic_adminq_post_wait`, `lif->index`,
`lif->ionic->pdev` (ernic_p4). None of those takes or needs the instance lock.

Every interaction found between patch 1 and the two:

- **A1. `ib_get_eth_speed()` now takes this netdev's instance lock.** `ionic_query_port()`
  calls it, so every `ib_query_port()` (sysfs, `rdma link`, `ibv_devinfo`, the IB cache update
  work, `ib_register_device()` at probe) does `rtnl_lock()` -> `netdev_lock_ops(ionic)` ->
  `ionic_get_link_ksettings()`. Before patch 1 it was RTNL only. It can only wait: no ionic
  path waits for an RDMA lock while holding the instance lock. It waits behind a long holder
  (FW recovery, a queue reconfigure, a bind), with RTNL held meanwhile.
- **A2. The aux devices are registered and unregistered outside the instance lock, for both.**
  p4 put its device into the same two functions (`ionic_auxbus_add_one("p4")` after
  `"rdma"`, `ionic_auxbus_del_one(&lif->p4_adev)` before the RDMA one, both under
  `adev_lock`), so `ernic_p4_probe()`/`_remove()` run where the RDMA probe/remove run. This
  matters for RDMA, whose probe reaches A1 (it would deadlock under the instance lock); for
  ernic_p4 it only keeps `misc_register()`/`misc_deregister()` out from under a netdev lock.
- **A3. Order at FW down / PCI reset, unchanged:** `FW_RESET` set, netdev detached, **aux
  devices unregistered**, then instance lock + `queue_lock`, queues and the adminq NAPI torn
  down. So RDMA and P4 are gone before `napi_disable_locked(&adminqcq->napi)`. A P4 ioctl or
  RDMA admin command already inside `ionic_adminq_post_wait()` is not waited for by
  `misc_deregister()`/the RDMA remove path any more than before (pre-existing; it ends in the
  adminq's own FW-reset/timeout handling).
- **A4. Order at recovery:** patch 8 moves `clear_bit(IONIC_LIF_F_FW_RESET)` from just after
  the unlocks to just before them. `ionic_auxbus_register()` still comes after both, so the
  RDMA and P4 probes still see the flag clear and a live adminq, as before.
- **A5. The adminq NAPI** that completes RDMA's and P4's commands is now added, enabled and
  disabled with the `_locked` helpers, in `ionic_lif_init()`/`_deinit()` (which take the lock
  for probe and remove) and the `_locked` variants (recovery). Its poll and `adminq_lock` are
  untouched.
- **A6. `ionic_intr_alloc()` (RDMA EQs) and the queues' interrupts** share `ionic->intrs`;
  the series changes neither side's locking. `ionic_queue_mem_alloc()` clears
  `IONIC_QCQ_F_INTR` on the shadow queue, so a bind allocates no interrupt.
- **A7. Symbol versions.** Patches 3, 4, 5 and 7 change `struct ionic_rx_stats`,
  `struct ionic_buf_info`, `struct ionic_queue` and `struct ionic_qcq`, so the CRC of every
  export taking `struct ionic_lif *` changes (`ionic_adminq_post_wait`: base, b1, b2
  `0xb2e51535`; b3 `0x8ba771f6`; b4 `0x085ac96e`; b5, b6 `0x36ef9260`; b7, b8 `0x7b937cd7`;
  `ionic_intr_alloc`/`_free` change again at b8, whose new include completes more types in
  `ionic_lif.c`; from `build/*/mod/Module.symvers`). **ionic_rdma and ernic_p4 must be
  rebuilt against this ionic's `Module.symvers`**, as for devmem-tx; with patches 1 and 2
  alone the CRCs are the base's. `struct ionic_lif` holds only pointers to the changed
  structures, and what ernic_p4 reads (`index`, `ionic`) is in front of them.
- **A8. A bind or unbind restarts every queue of the LIF** (the stub), which sends
  `Q_CONTROL` for all of them and detaches/attaches the netdev. The RDMA port state follows
  `netif_running()` and carrier, which a restart does not change; what the emulator does to
  RDMA or P4 traffic of that LIF meanwhile is as for an `ethtool -G` today, and was not run
  with either.
- **A9. `IONIC_RXQ_F_HDS` vs srq/p4 bits.** `BIT_ULL(17)` is a *queue* feature
  (`enum ionic_rxq_feature`); srq 0004 adds identity fields and p4 adds `IONIC_LIF_CAP_P4`
  (`BIT(2)` of the LIF capabilities). No overlap. `ionic_rdma` reads
  `qtype_info[RXQ].features` only for `IONIC_QIDENT_F_EXPDB`.

Never run anywhere: ernic_p4 beside an ops-locked ionic (the v7.4 tree has no p4 patch), and
the SRQ ionic_rdma beside one (stack 2 ran v7.4's in-tree ionic_rdma).

## 5. Without header split requested: what is the same, and what is not

`lif->rxq_features` gets `IONIC_RXQ_F_HDS` in one place, `ionic_set_ringparam()`, and only
after `ionic_lif_hds_capable()`: `Q_IDENTIFY(RXQ).features` has both `IONIC_Q_F_2X_CQ_DESC`
and bit 17, and `max_sg_elems != 0`. The server offers that only with
`--nic-profile header-split`; with the default profile every explicit `tcp-data-split`
setting is refused, so no queue ever has the bit.

With the bit clear, **per packet nothing changes** (read in `O/tree`):

- `ionic_rx_clean()`: the `IONIC_RXQ_F_HDS` branch is not taken; copybreak and `build_skb`
  are chosen as before, and `napi_gro_receive()`/`napi_gro_frags()` follow them as before.
  `ionic_rx_csum()` makes the old decision (`RXCSUM` and `CALC` -> `CHECKSUM_COMPLETE`, else
  none; `csum_error` counted the same). The added `!skb_frags_readable()` test is false: the
  pool has no `PP_FLAG_ALLOW_UNREADABLE_NETMEM`.
- `ionic_rx_fill()`: `first_pool` is the queue's pool, lengths as before, `remain_len`
  reduced as before; when every buffer is allocated the doorbell and deadline are written as
  before. `ionic_rx_empty()`: `hdr_pool` is NULL.
- `ionic_rxq_pools_create()`: the same `page_pool_params` as the inline code it replaces; no
  header pool. `ionic_qcq_alloc()` sets `q.features` at allocation instead of after it.
- Rx buffers are tracked as `netmem_ref`, all page-backed (patch 4, no functional change).
- Tx datapath: untouched.

**It is not "exactly as before" outside the datapath.** These are active whatever the device
offers:

1. Patch 1: the netdev is ops locked. ndos and ethtool ops run under the instance lock; all
   ethtool ops but ring, channels and get_link lose RTNL; rx_mode changes reach the device
   synchronously from the async op instead of from ionic's deferred work; the link check, Tx
   timeout work, FW down/up, hwstamp replay and PCI reset take the instance lock;
   `ib_get_eth_speed()` takes it (A1).
2. Patch 6: `supported_ring_params` claims `tcp-data-split`. `ethtool -g` still shows it as
   n/a (the driver reports unknown), and `-G ... tcp-data-split on|off` still fails with
   `-EOPNOTSUPP`, now from the driver with "Device does not support tcp-data-split".
3. Patch 7, only when an Rx buffer allocation fails: the doorbell is rung for what was
   posted, `rx_N_alloc_err` counts, and a 10 ms timer reschedules NAPI if the ring is empty.
   Before, the fill returned silently. Every qcq carries an (unarmed) timer.
4. Patch 8: `netdev->queue_mgmt_ops` is set, so a bind attempt gets as far as the core's
   "tcp-data-split is disabled" instead of failing for lack of queue ops;
   `ionic_swap_queues()` gives the pools of the temporary queue no NAPI (ring and channel
   changes); `ionic_restart_lif()` clears `FW_RESET` before its unlocks.
5. New `ethtool -S` names (`rx_csum_unnecessary`, `rx_hds_split`, `rx_hds_hbo`,
   `rx_unreadable_drop`, per queue and summed), all 0.

## 6. What was run

Host x86_64. Builds in the `devmem-kbuild:1` container, **gcc (Ubuntu 15.2.0-16ubuntu1)
15.2.0**, the compiler that built the kernel.

| Check | Result | Evidence (`O/evidence/`) |
| --- | --- | --- |
| Base = v7.2.6 + srq 0001-0011 + p4 + devmem-tx 0001-0007 (`git apply`), vs the tree stack 3 runs | identical | `mkbase.log` |
| Stages: each mainline patch with `patch -F2`; 0 rejects; one fuzz-1 hunk (`ionic_phc.c`) | b1..b8 | `mkstages.log` |
| Every file of every stage = the mainline stage `sN` + the base's own differences from `s0` | b0..b8 | `mkstages.log` |
| Each patch file: `git apply --check`, `git apply --whitespace=error-all` in order on a fresh base; result = its stage; `git mailinfo`, `git mailsplit` (1 message); mailinfo's diff reverse-applies; 0 Signed-off-by; backport paragraph present; final tree = `b8` | 8/8, rc 0 | `verify.log` |
| Hunks compared with the mainline patches | 2..8 identical but for line numbers (and `Documentation/`); 1 differs in one context line | `verify.log` |
| What the series needs: applies on srq+p4+tx and srq+tx; not on srq+p4 (patch 5 fails), not without srq/0010 (patch 4 fails), not on plain v7.2.6+tx (patch 4) | as expected | `combos.log` |
| `checkpatch.pl --strict` (v7.2.6's) | each patch: 1 error, "Missing Signed-off-by", 0 warnings, 0 checks | `checkpatch.log` |
| **W=1**, `make -C test-rocm102/kbuild/out M=<mod>` (the dkms.conf make line + W=1) and `drivers/ernic_p4`'s Makefile with `IONIC_SRC`/`IONIC_SYMVERS`, at base, after each of the 8 patches, and from the tree made by the patch files | rc 0, **27 CC, 5 LD, 0 warnings, 0 errors every time**; `ionic.ko`, `ionic_rdma.ko`, `ernic_p4.ko` produced, vermagic `7.2.6-devmemp2p` | `build-w1-*.log`, `build-summary-w1-*.txt` |
| **sparse** `C=2 W=1`, same ten trees | 22 files checked each, 0 warnings. A negative control (an injected `__le32` mismatch) is reported | `build-sparse-*.log`, `build-sparse-NEGATIVE-CONTROL.log` |
| W=1 against the **stock guest config** (`config-7.2.6-070206-generic`, `NET_SHAPER=y`; private header tree `O/kprep-generic` from the pristine v7.2.6 worktree) | all 22 source objects compile with 0 diagnostics at all ten trees. The `.ko` files can't be linked there (no `Module.symvers` for that config: the generated `.mod.c` fails) | `build-generic-*.log`, `build-summary-generic-*.txt`, `kprep-generic.log` |

The kernel tree used for the module builds is `/var/tmp/ionic-devmem/test-rocm102/kbuild/out`
(**7.2.6-devmemp2p**: v7.2.6 + the one-commit p2p backport, the guest config reduced with
localmodconfig), with its source `kbuild/src`, both **mounted read-only** in the container;
nothing was written there. `sparse` is the git build in `tools/sparse-src`: the packaged
0.6.4 fails v7.2.6's `scripts/checker-valid.sh` and kbuild then silently skips the check (my
first sparse run was that; the script now fails when no file is checked).

NOT RUN: `git am` (it commits); any load, boot, bind or traffic; lockdep, KASAN (the guest
kernels have neither); a `.ko` link against the stock kernel's `Module.symvers`; clang;
`dkms build`; `fetch-ionic-sources.sh`; `ionic-mod.sh` with these patches; the stock selftest.

## 7. Most likely wrong at runtime on v7.2.6, most likely first

1. **Patch 1 with nothing to report a mistake.** The guest kernels (`7.2.6-070206-generic`
   and `7.2.6-devmemp2p`) have no lockdep and no `DEBUG_NET`: every `netdev_assert_locked()`
   is compiled out, and an inversion shows only as a hung `ip`, `ethtool`, `rdma` or
   `modprobe`. The v7.4 run under lockdep covered the same driver code, but not: the rx_mode
   work with RTNL held (3.2), the SRQ ionic_rdma, ernic_p4, this image's boot flow (the
   rename, the refused autoload of the baked `ionic_rdma`/`ernic_p4`, `guest-prep.sh`,
   `quick-start test`). If anything hangs: `echo t > /proc/sysrq-trigger` and
   look for who holds `dev->lock`/RTNL; better, add `PROVE_LOCKING` + `DEBUG_NET` to the
   devmemp2p config for the bring-up.
2. **RDMA while ionic holds the instance lock for long** (A1): `ibv_devinfo`/`rdma link`/the
   quick-start RDMA tests during a bind, an `ethtool -G/-L`, an MTU change. Expect a wait,
   not a failure; a hang here is finding 1.
3. **A bind/unbind under RDMA or P4 load** (A8): never run. The whole-LIF queue restart is
   the same sequence as `ethtool -G`, but it now happens at every `ncdevmem` start and exit.
4. **Where a GPU buffer ends up.** On the stock kernel the bind attaches statically, so an
   amdgpu GEM or hipMalloc buffer is moved to system memory and the NIC *writes* there (seen
   for TX: system RAM addresses; and binding a hipMalloc buffer evicts the KFD process). Only
   `7.2.6-devmemp2p` (r102-1) keeps it in VRAM, and then the emulator has to DMA-**write** to
   GPU BAR0 addresses, which only TX (reads) has done. Size the buffer: a header-split queue
   wants a page per descriptor (ring default 1024 -> 4 MiB, maximum 16384 -> 64 MiB) on top of what the application
   holds; with less `rx_N_alloc_err` counts and the queue runs partly filled.
5. **Tooling against the v7.2.6 uAPI.** The RX runs used a modified v7.4 selftest `ncdevmem`
   (`-n`: no flow steering, which the emulator lacks; `test-rx/bin/ncdevmem`). Stack 3's
   `~/devmem/ncdevmem` did TX on 7.2.6; the RX/listen path with `-n` has not run there. A
   bind refused with an unknown-attribute error is the tool, not the driver.
6. **rx_mode now synchronous** (section 5, item 1): promiscuous/allmulti/multicast changes
   issue adminq commands in the caller. A filter that stops being programmed (multicast,
   IPv6 neighbour discovery) would show as lost multicast, not as an error.
7. **Core bugs v7.2.6 still has** (section 2): a pool destroyed while frames are returned in
   bulk (`page_pool_recycle_ring_bulk()`, `PAGE_POOL_STATS=y` in the guest) and a bind
   destroys pools every time; `TCP_ZEROCOPY_RECEIVE` on a devmem socket. Low.
8. Everything `test-rx.md` section 5 leaves open is open here too (a firmware that really
   stops, the stub's failure paths without the test-only knobs, two queues on one dma-buf).

### What to exercise first, in this order

Check `dmesg` after every step (there is no lockdep to do it for you) and that `ip -br link`,
`ethtool rocm-ernic0` and `ibv_devinfo` still return.

1. **Patch 1 only, compat server (as the stack runs now).** Load (section 8). `ip link set
   down/up` x5; `guest-prep.sh`/fleet address; `qs-test.sh` base; `srq-test.sh`; `qs-test.sh
   p4 --p4-test` (then `stack.sh cycle`); a dma-buf RDMA run; `ethtool -G rx 512`, `-L combined
   1` and back, MTU 9000 and back, each **while `ib_send_bw` runs**; `tcpdump -i rocm-ernic0`
   start/stop (promiscuous via the async op) and `ip maddr add`; `ethtool -c/-a/-x/-S/-k`;
   `rmmod ernic_p4 ionic_rdma ionic` while up, reload, x3; devmem TX smoke
   (`devmem-tx/smoke.sh`). A PCI function reset (`echo 1 > .../reset`) last: never
   warm-reboot.
2. **Whole series, compat server.** The same, plus: `ethtool -g` shows no tcp-data-split
   value; `ethtool -G rocm-ernic0 tcp-data-split on` fails with "Device does not support
   tcp-data-split"; hashed TCP both ways; the 35-case TX matrix. This is the "nothing
   changed" claim of section 5.
3. **Header-split server** (`stack.sh down; verify/vserver.sh header-split; stack.sh up`),
   split off: hashed TCP/UDP, no "hw csum failure", `rx_csum_complete` rises.
4. `ethtool -L rocm-ernic0 combined 1; ethtool -G rocm-ernic0 tcp-data-split on`: hashed TCP
   both ways, `rx_hds_split` and `rx_csum_unnecessary` rise, `rx_N_dropped` 0; MTU 9000;
   ifdown/ifup; RDMA and SRQ tests with split on.
5. Bind with udmabuf first: `ncdevmem -n -l -s 10.102.100.11 -p 5201 -f rocm-ernic0 -t 0 -q 1
   -v 7 -L` on r102-1, a plain TCP sender on r102-2; 0 `SCM_DEVMEM_LINEAR`, payload validated.
   Then kill it, bind again, ifdown/ifup with the binding, `ib_send_bw` across a bind.
6. Only then GPU buffers, stock kernel first (expect system RAM), then `kernel-select.sh p2p`
   on r102-1.

## 8. Building and loading in a guest (NOT RUN by me)

Stack 3's own tool does it; it re-applies the series to v7.2.6 from its `git archive` of
2d3a3d02 and the directory given, the way `mkbase.sh` + `verify.sh` did (`git apply` in a
plain directory):

```bash
S=/var/tmp/ionic-devmem/test-rocm102/scripts
O=/var/tmp/ionic-devmem/overnight2/rx-backport

# patch 1 only (DIR:N takes the first N patches of DIR)
$S/ionic-mod.sh stage rxbp-p1 srq p4 devmem-tx $O/patches:1
$S/ionic-mod.sh build rxbp-p1          # both guests; "build rxbp-p1 1" for r102-1 only
$S/ionic-mod.sh load  rxbp-p1          # rmmod ernic_p4 ionic_rdma ionic; insmod; verify

# the whole series
$S/ionic-mod.sh stage rxbp srq p4 devmem-tx $O/patches
diff -r /var/tmp/ionic-devmem/test-rocm102/ionic-mod/rxbp/tree $O/tree    # expect no output
$S/ionic-mod.sh build rxbp && $S/ionic-mod.sh load rxbp

# back to what runs now / to the baked modules
$S/ionic-mod.sh load devmem
$S/ionic-mod.sh restore
```

To use my staged tree as it is instead of re-applying, copy it into place and skip `stage`:
`mkdir $B/ionic-mod/rxbp && cp -r $O/guest/. $B/ionic-mod/rxbp/` (with
`B=/var/tmp/ionic-devmem/test-rocm102`), then `build` and `load` as above.

What `build` and `load` run in the guest, for doing it by hand (from `ionic-mod.sh`):

```bash
mkdir -p ~/ionic-mod/rxbp && cd ~/ionic-mod/rxbp && tar -xzf /tmp/ionic-mod-rxbp.tgz   # = O/guest/stage.tgz
cp /usr/src/ionic-ernic-7.2.6/Kbuild mod/Kbuild
make -s -C /lib/modules/$(uname -r)/build M=$PWD/mod -j$(nproc) modules      # ionic.ko, ionic_rdma.ko
mkdir p4 && cp -r /usr/src/ernic-p4-0.2.0/. p4/
make -s -C p4 KDIR=/lib/modules/$(uname -r)/build IONIC_SRC=$PWD/tree \
     IONIC_SYMVERS=$PWD/mod/Module.symvers                                    # ernic_p4.ko
sudo dmesg -C
for m in ernic_p4 ionic_rdma ionic; do [ -e /sys/module/$m ] && sudo rmmod $m; done
sudo modprobe ib_core                      # what "modinfo -F depends" names
sudo insmod mod/ionic.ko; sudo udevadm settle   # udev's modprobe of the baked pair is refused
sudo insmod mod/ionic_rdma.ko; sudo insmod p4/ernic_p4.ko
sudo ip link set rocm-ernic0 up; sudo ip addr add 10.102.100.1N/24 dev rocm-ernic0
```

Not persistent: a cold cycle returns to the baked DKMS modules. On r102-1 with
`7.2.6-devmemp2p` selected, `build` uses that kernel's headers (`uname -r`). Header split
needs the server profile: `$S/stack.sh down; $S/verify/vserver.sh header-split; $S/stack.sh
up` (`vserver.sh poc-rel` to go back).

## 9. For the repository (nothing was written there)

- The series belongs beside `devmem-tx`, e.g. `ansible/files/patches/devmem-rx-v7.2.6/`
  (or replacing `devmem-rx/`'s claim that the series does not apply to `IONIC_KERNEL_REF`),
  applied **after** `srq`, `p4` and `devmem-tx`: `combos.log` shows what it needs.
- `devmem-rx/README.md` says a backport "would be the same exercise"; it now exists, built
  and unrun. The README's "Carries a bug the base series fixes" section is already obsolete
  (rx-xdp-frag-fix.md section 8).
- Callers that would have to pass the directory are the ones `impl-backport.md` lists for
  `devmem-tx`, plus this one after it. `ernic_p4_dkms` and the SRQ DKMS role must then build
  against the same ionic (A7).
