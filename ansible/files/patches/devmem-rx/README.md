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

**`v7.4-rc3`**, on top of the netmem Tx series in
[`../devmem-tx`](../devmem-tx) in its mainline form.

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

**Not tested:** ionic hardware, a firmware that really stops, a
device-reported link flap, the upstream `devmem.py` selftest (ionic has
no flow steering, so `ncdevmem` must be run with `-n`), and any kernel
other than `v7.4-rc3`.

## Retiring this directory

Delete it if ionic firmware gains header split and the interface in
`0002` is replaced by the real one, or if the work is abandoned.
