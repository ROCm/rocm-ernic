# ionic patches for receiving into device memory, on the pinned kernel

The eight patches of [`../devmem-rx`](../devmem-rx) backported to the
sources this project's guests build ionic from, so that a guest can bind
a dma-buf to an ionic receive queue (`NETDEV_CMD_BIND_RX`) and receive
into it with `recvmsg(MSG_SOCK_DEVMEM)`.

**A proof of concept, not a series to deploy.** No ionic firmware splits
headers; the device these were developed against is this emulator in its
`header-split` mode (`--nic-profile header-split`), and `0002` holds
that invented interface's definitions. Everything
[`../devmem-rx/README.md`](../devmem-rx/README.md) says about the series
applies here — this directory changes only which kernel it applies to.

## Base

**`v7.2.6`**, the ref `IONIC_KERNEL_REF` pins in
[`cmake/ErnicKernelModule.cmake`](../../../../cmake/ErnicKernelModule.cmake),
with three series applied first, in this order:

1. [`../srq`](../srq) `0001-0011`. Not optional: `0004` needs the XDP
   frag offset fix, which is `srq/0010` here, and does not apply without
   it (on purpose — see `../devmem-rx/README.md`).
2. [`../p4`](../p4).
3. [`../devmem-tx`](../devmem-tx) `0001-0007`, **without its `0008`**
   (next section).

`git apply` was checked on every other combination: the series also
applies on `v7.2.6` + srq + devmem-tx, and **fails** on srq + p4 without
devmem-tx (`0005`), without `srq/0010` (`0004`), and on plain `v7.2.6` +
devmem-tx (`0004`).

Give it as the **last** `--patches-dir`, after those three. Note that
`--patches-dir` replaces rather than adds, and that the base series in
[`../`](../) is not in the list: `srq/0009-0011` are its copies, so
naming both would apply the same three patches twice.

### Why `devmem-tx/0008` has to be left out, and what that costs

[`../devmem-tx/0008`](../devmem-tx) (the frag-limit fix) adds
`tx_unreadable_reseg` to `struct ionic_lif_sw_stats` in `ionic_lif.h`
and to `ionic_lif_stats_desc[]` in `ionic_stats.c` at exactly the lines
where `0005` here adds `rx_hds_split`, `rx_hds_hbo` and
`rx_unreadable_drop`. With `0008` applied first, `0005` fails on both
files and `0008` of this series fails on `ionic_lif.c` behind it. The
order that works, and the one that was run, is `devmem-tx` `0001-0007`,
then this series, then
[`../devmem-tx/on-devmem-rx/0009`](../devmem-tx/on-devmem-rx).

[`fetch-ionic-sources.sh`](../../scripts/fetch-ionic-sources.sh) applies
every `*.patch` in a directory it is given, so
`--patches-dir .../devmem-tx` brings `0008` along and **cannot be used
for a guest that also wants this series**. Until the two are rebased
onto one another, a Tx+Rx guest needs `devmem-tx/0001-0007` copied into
a directory of their own.

For the same reason there is **no `ionic-patches` leg in
[`ernic-driver-build.yml`](../../../../.github/workflows/ernic-driver-build.yml)
for this series**, as there is for `devmem-tx`: that job builds a base
out of whole directories, and the base this series needs —
srq + p4 + devmem-tx without one of devmem-tx's files — is not something
a `--patches-dir` list can express. Give the two series one shared order
and the leg becomes possible.

| File | What it does |
| --- | --- |
| `0001-ionic-run-the-netdev-ops-under-the-instance-lock.patch` | Makes ionic an ops-locked driver. Independent of the rest, and the part most needing review. |
| `0002-ionic-add-placeholder-definitions-for-rx-header-split.patch` | The invented device interface. **Must not go upstream.** |
| `0003-ionic-trust-the-devices-l4-checksum-on-header-split-queues.patch` | `CHECKSUM_UNNECESSARY` where the payload may be unreadable. |
| `0004-ionic-track-rx-buffers-as-netmem.patch` | Rx buffer bookkeeping as netmem. No functional change. |
| `0005-ionic-put-headers-in-the-linear-area-on-header-split-queues.patch` | The header-split datapath. |
| `0006-ionic-support-the-tcp-data-split-ring-parameter.patch` | `ethtool -G tcp-data-split`, and the XDP and timestamp exclusions. |
| `0007-ionic-keep-an-rx-queue-going-when-its-buffers-run-out.patch` | A dma-buf smaller than the ring is ordinary, not an error. |
| `0008-ionic-add-queue-management-ops-and-allow-unreadable-rx-payload.patch` | Queue management ops and the payload pool's memory provider. |

There is no cover letter here; `../devmem-rx/0000-cover-letter.patch` is
the one for the series, and says what only a device vendor can answer.

## How the backport differs from the v7.4-rc3 form

**No code had to change.** Seven of the eight diffs are the mainline
diffs but for line numbers and the two `Documentation/` hunks, which the
DKMS sources do not have; `0001` differs in one context line, because
`ionic_lif_hwstamp_replay()` has no `hw_features` test in `v7.2.6`. The
end state is the mainline series' end state plus exactly the lines in
which `v7.2.6` + srq + p4 already differed from `v7.4-rc3`.

What differs is the core around the driver. Each patch's message ends
with a `[ rocm-ernic: ... ]` note; two differences are worth repeating:

- **`v7.2.6` only warns.** An ops-locked netdev that keeps
  `ndo_set_rx_mode` makes `register_netdevice()` emit a `netdev_WARN`
  there, where `v7.4` refuses it with `-EINVAL` (`05fe748456ea`). `0001`
  converts the driver to `ndo_set_rx_mode_async` anyway, so that no
  probe taints the kernel with a warning.
- **The rx_mode async op runs under RTNL here.** `netdev_work_proc()`
  takes `rtnl_lock()` and then the instance lock on `v7.2.6`; `v7.4`
  takes the instance lock alone (`5a81c72ef9ad`). So a multicast join, a
  `tcpdump` promiscuous toggle or `ip link set ... allmulti` now issues
  its adminq commands with RTNL held. That is no new lock order (RTNL →
  instance is the core's order everywhere) and no deadlock, but against
  a firmware that stops answering each one waits for its adminq timeout
  with RTNL held.

## `ionic_rdma` and `ernic_p4` must be rebuilt against this ionic

`0003`, `0004`, `0005` and `0007` change `struct ionic_rx_stats`,
`struct ionic_buf_info`, `struct ionic_queue` and `struct ionic_qcq`, so
**the CRC of every export taking a `struct ionic_lif *` changes at five
of the eight patches** (`ionic_adminq_post_wait` takes four distinct
values across the series; `ionic_intr_alloc`/`_free` change again at
`0008`). A `ionic_rdma.ko` or `ernic_p4.ko` built against an earlier
`Module.symvers` is refused at load with "disagrees about version of
symbol". Build all three from one tree, as for `devmem-tx`, and list
this directory in `ernic_p4_dkms_patches_dirs` and in the SRQ DKMS
role's patch directories after the other three.

**Neither module has ever run beside an ops-locked ionic anywhere but
here**: the `v7.4-rc3` tree has no P4 patch, and stack 2's runs used
`v7.4`'s in-tree `ionic_rdma`, not the SRQ one. The interaction that
matters is `ib_get_eth_speed()`, which `ionic_query_port()` calls: with
`0001` every `ib_query_port()` takes RTNL and then *this netdev's*
instance lock, where before it took RTNL alone. It can only wait — no
ionic path waits for an RDMA lock while holding the instance lock — but
it waits behind a long holder (firmware recovery, a queue reconfigure, a
bind) with RTNL held meanwhile.

## A bind or unbind restarts every queue of the LIF

The queue-management stub `0008` adds restarts the whole LIF rather than
the one queue: a bind or an unbind sends `Q_CONTROL` for every queue and
detaches and re-attaches the netdev. That is the same sequence an
`ethtool -G` performs today, but it now happens at every `ncdevmem`
start and exit. What it does to RDMA or P4 traffic on that LIF meanwhile
was **not** run with either.

## What was tested

Two guests of this emulator on `v7.2.6` with the peer-to-peer dma-buf
backport (`7.2.6-devmemp2p`), the server in `header-split` mode, with
`ionic_rdma` and `ernic_p4` rebuilt against this ionic:

- Built in both guests, with `W=1` and sparse and with no warning, at
  the base and after each of the eight patches, against the guest
  kernel's headers and against the stock guest config; `checkpatch.pl
  --strict` reports only the missing `Signed-off-by:`; each patch file
  reproduces its stage and parses with `git mailinfo`.
- `0001` alone was loaded and exercised first. Then the whole series:
  ops-locking rounds of ifdown/ifup, ring, channel and MTU changes, XDP
  attach and detach, 30 ethtool operations, promiscuous, `tcpdump`,
  allmulti, 41 multicast addresses, MAC change, IPv6 neighbour discovery, `rdma link` and
  netlink dumps — idle, under `iperf3` and `ib_send_bw` load, with PCI
  function resets, and with `rmmod`/reload cycles. About 1000 rounds of
  `ibv_devinfo -v` + `rdma link` + an open of `/dev/ernic-p4` raced
  against about 2300 ring/channel/MTU/ifdown operations: 0 failures,
  worst round 59 ms.
- A **lockdep** kernel (`PROVE_LOCKING`, `DEBUG_ATOMIC_SLEEP`,
  `DEBUG_NET`, `DEBUG_MUTEXES`, `DEBUG_SPINLOCK`) in one guest for one
  boot, covering the ops-locking rounds, header split and devmem receive
  in both roles: **nothing reported**, `debug_locks` still 1.
- Header split with ordinary pages: 44,199 of 44,199 data skbs with 52
  linear bytes and one frag and `CHECKSUM_UNNECESSARY`; IPv4, IPv6, UDP,
  ARP, IP fragments, 1-to-5-byte segments, GRO off, `rx` checksum off,
  ring and channel changes, receiver at MTU 9000. 0 "hw csum failure"
  lines, 0 `TcpInCsumErrors`.
- Devmem receive into a udmabuf: first light, then 1 MiB to **1 GiB**
  (742,429 cmsgs, 0 through the linear area, 0 validation failures);
  devmem Tx into devmem Rx at four sizes with the receiver's dma-buf
  hashing equal to a copy-mode control; `kill -9` mid-stream;
  ifdown/ifup, ring, MTU and channel changes with a live binding; 20
  bind/transfer/exit cycles; receive with the fp8 P4 program loaded. 0
  leaked page pools and 0 dma-buf objects after every case, including
  after about 320 binds in the stress runs.
- 157 and 160 bind/unbind cycles raced against ring, channel, MTU,
  ifdown/ifup, PCI resets and netlink dumps over 90 s, each guest.
- The upstream quick-start tests on these modules with split off and
  split on: base, SRQ, `--rocm-rdma-mode dmabuf`, `--p4-test`, all PASS.
- With the feature absent (the `compat` server) the driver behaves as
  before the series: `ethtool -g` shows `TCP data split: n/a`, `-G
  tcp-data-split on|off` is refused, and a bind fails with
  "tcp-data-split is disabled".

**Not tested:** ionic hardware; GPU buffers as the *receive* dma-buf on
this series alone (that is in the end-to-end runs, which used it
together with the frag-limit fix); jumbo frames across the wire (the
test stack's L2 is MTU 1500); VLAN; IPv6 devmem receive; two bound
queues; `rmmod` with a live binding; injected queue-op failure paths; a
firmware that really stops or a device-reported link flap, neither of
which the emulator can do; KASAN or kmemleak, which the guest kernels do
not have; `dkms build`, `fetch-ionic-sources.sh` or `git am` with these
files; and the upstream `devmem.py` selftest (ionic has no flow
steering, so `ncdevmem` must be run with `-n`).

The report is `impl-rx-backport-7.2.6.md` (the backport) and
`rocm102-rx.md` (the runs) in
[`docs/devmem/notes`](../../../../docs/devmem/notes).

## Retiring this directory

Delete it when `IONIC_KERNEL_REF` moves to a kernel whose ionic needs a
different backport, when `../devmem-rx` is replaced by something
upstream, or when the work is abandoned. `../devmem-rx` stays as the
`v7.4-rc3` reference form of the same series.
