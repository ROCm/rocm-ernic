# ionic patches for receiving into device memory (devmem TCP)

Eight patches against the upstream Linux ionic Ethernet driver that let a
guest bind a dma-buf to an ionic receive queue (`NETDEV_CMD_BIND_RX`) and
receive into it with `recvmsg(MSG_SOCK_DEVMEM)`, on a device that splits
each segment's headers from its payload.

**A proof of concept, not a series to deploy.** No ionic firmware splits
headers. The device these were developed against is this emulator, in
its `header-split` mode (`--nic-profile header-split`), which implements
an interface invented for the purpose; `0002` holds that interface's
definitions, marked as placeholders. `IONIC_RXQ_F_HDS` is simply the
next free bit in `ionic_if.h`, and a device that already uses it would
be taken for one that splits headers.

## Base

**`v7.4-rc3`**, with two things applied first, in this order:

1. the XDP frag offset fix, in its mainline form:
   [`0001-ionic-use-the-rx-buffers-page-offset-for-xdp-frags.patch`](../../../../tests/devmem/kernel-patches/ionic-mainline-fixes).
   It is the same change as
   [`../0003-ionic-use-the-Rx-buffer-s-page-offset-for-XDP-frags.patch`](..),
   which is against `v7.2.6`, and has the same patch-id, the first
   `prerequisite-patch-id` in the cover letter;
2. the netmem Tx series in its mainline form,
   [`../devmem-tx-mainline`](../devmem-tx-mainline).

That is not `IONIC_KERNEL_REF`, which
[`cmake/ErnicKernelModule.cmake`](../../../../cmake/ErnicKernelModule.cmake)
pins at `v7.2.6`, so **these do not apply to the sources
`fetch-ionic-sources.sh` fetches**, and no role or variable can build a
guest with them. `ionic_lif.c`, `ionic_txrx.c`, `ionic_ethtool.c`,
`ionic_if.h`, `ionic_lif.h`, `ionic_phc.c` and `ionic_bus_pci.c` all
moved between the two tags. A backport would be the same exercise
`devmem-tx` went through, and nothing in the v7.2.15 kernel's core is
missing for it: `netdev_queue_mgmt_ops`, `netdev_rx_queue_restart()`,
`PP_FLAG_ALLOW_UNREADABLE_NETMEM`, `ETHTOOL_RING_USE_TCP_DATA_SPLIT`,
`ndo_set_rx_mode_async` and `request_ops_lock` are all there.

They are kept here so the emulator's header-split mode and the driver
that drives it stay together.

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

`0000-cover-letter.patch` says what only a device vendor can answer.

## Needs the XDP frag offset fix

`0004-ionic-track-rx-buffers-as-netmem.patch` converts the XDP frag loop
in `ionic_run_xdp()` to netmem. The series as first carried here was
made without the fix above, and `0004` kept both halves of the bug in
netmem form: the frag filled at offset `0`, and synced at the *first*
buffer's offset. A frame of more than two buffers sent with `XDP_TX` or
`XDP_REDIRECT` could carry another frame's bytes, or what the page held
before.

The series was regenerated on top of the fix. `0004` now replaces the
fixed lines with their netmem form, `bi->page_offset` in both calls:

```c
page_pool_dma_sync_netmem_for_cpu(rxq->page_pool, bi->netmem,
                                  bi->page_offset, frag_len);
skb_frag_fill_netmem_desc(frag, bi->netmem, bi->page_offset, frag_len);
```

so **`0004` does not apply without the fix, on purpose**: a series that
applied without it would be the bug again. `0004` is the only patch
whose change differs from the first version, by those two calls; it is
still no functional change relative to its base.

The fix is not upstream, so a rebase of this series onto a newer
mainline has to bring it along until it is. Nothing applies this series,
and the `ionic-patches` workflow, which checks that the `v7.2.6` tree
has the fix, does not cover these patches.

## What was tested

Two guests of this emulator, on a `v7.4-rc3` kernel with KASAN and
lockdep, the emulator in `header-split` mode:

- Header split with ordinary pages: headers in the linear area, payload
  in frags, `CHECKSUM_UNNECESSARY`, GRO, for IPv4, IPv6 and VLAN, at MTU
  1500 and 9000.
- Receive into a dma-buf, 1 MiB to 1.5 GB validated, from an ordinary
  sender and from a devmem transmitter, with no payload arriving through
  the linear area.
- The binding surviving ifdown/ifup, ring resize, MTU change, channel
  change, PCI function reset and a simulated firmware reset mid-stream.
- Teardown with a live binding: no leaked page pool, no KASAN or lockdep
  report.
- With the mode off, or `tcp-data-split` off, behaviour as before the
  series.

All of that ran on the series as it was before it was regenerated on the
XDP frag offset fix. On the regenerated patches these were run, with a
module that differs from the earlier one in `ionic_txrx.c` only:

- An `xdp.frags` program echoing 8000-byte UDP datagrams at MTU 9000
  with `tcp-data-split` off, every echo compared on the wire with what
  was sent: 10,000 of 10,000 intact with `XDP_TX` and with
  `XDP_REDIRECT`, with the emulator's mode on and off. The module built
  from the series as it was had 3728 of 7458 and 4011 of 8023 echoes
  wrong, each in its last 426 bytes.
- One round of the ifup/ifdown, ring, channel, MTU and XDP operations
  with the mode off.
- With the mode on: TCP of 64 MiB each way with `tcp-data-split` on and
  ordinary pages; 64 MiB received into a dma-buf from an ordinary
  sender; and 100,000 small sends from a devmem transmitter into a
  dma-buf, in each direction between the guests. Payloads validated, no
  payload through the linear area, no KASAN or lockdep report.
- Each patch applies in order to `v7.4-rc3` plus the two prerequisites
  and builds with `W=1` and sparse without a warning; `checkpatch
  --strict` reports only the missing `Signed-off-by`.

The reconfiguration, reset and teardown cases, IPv6, VLAN and the larger
transfers were **not** re-run on the regenerated patches.

**Not tested:** ionic hardware, a firmware that really stops, a
device-reported link flap, the upstream `devmem.py` selftest (ionic has
no flow steering, so `ncdevmem` must be run with `-n`), and any kernel
other than `v7.4-rc3`.

## Retiring this directory

Delete it if ionic firmware gains header split and the interface in
`0002` is replaced by the real one, or if the work is abandoned.
