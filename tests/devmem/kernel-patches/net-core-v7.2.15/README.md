<!--
Copyright (C) Advanced Micro Devices, Inc.
SPDX-License-Identifier: GPL-2.0-or-later
-->

# net core: detach devmem Tx bindings when their netdev goes (v7.2.15)

One patch for the Linux networking **core**, not the ionic driver:

    0001-net-devmem-detach-TX-bindings-on-NETDEV_UNREGISTER.patch

It is a hand-backport to `v7.2.15` of a fix net-next already has. It is
here because the devmem Tx tests found the bug it fixes, and a `v7.2.x`
guest kernel that offers devmem Tx has it. **It has not been booted**;
see "What was checked".

## What it fixes

A use-after-free, followed by an oops, in `netdev_nl_sock_priv_destroy()`.

A devmem TCP **TX** binding (`NETDEV_CMD_BIND_TX`) keeps a bare
`struct net_device *` in `binding->dev` (and an opaque `binding->vdev`).
An **RX** binding has that pointer cleared from
`mp_dmabuf_devmem_uninstall()` when the last RX queue it is installed on
is torn down. A TX binding has no bound RX queue, so that path never
runs for it, and nothing in netdev unregistration detaches it. After the
device is unregistered and freed, `binding->dev` dangles. The binding
stays on the netlink socket's binding list, and TCP retransmits of
already-queued devmem skbs can keep it alive long past the unregister.
When the binder process closes its netlink socket,
`netdev_nl_sock_priv_destroy()` reads `binding->dev` and calls
`netdev_hold()` on the freed memory:

    BUG: KASAN: use-after-free in netdev_nl_sock_priv_destroy+0x1b1/0x1e0
     genl_release -> netlink_release -> __sock_release -> sock_close -> __fput
    BUG: unable to handle page fault for address: ffff88829fd34000
    Oops: Oops: 0002 [#1] SMP KASAN NOPTI
    RIP: 0010:netdev_nl_sock_priv_destroy+0xbd/0x1e0

## Reproduction

On a guest with a devmem-TX-capable ionic (the rocm-ernic userspace
emulated NIC with the devmem TX driver series), start a devmem TX stream
with a live binding, then `rmmod ionic` a few seconds in. The unload
itself succeeds; the sender's next `sendmsg()` fails with `ENETUNREACH`
(or it blocks in `sk_stream_wait_memory()` if it has no `SO_SNDTIMEO`).
The KASAN splat and the oops come when the sender process exits and
closes its netlink socket — including when it is killed with `SIGKILL`.
Write-ups: `test-tx-7.4-rc3.md` section 4.3 (finding F8), and
`test-tx-verification.md` section 3.1 and finding V3, in
[`docs/devmem/notes`](../../../../docs/devmem/notes).

## Which upstream commit this backports

Upstream has already fixed this in **net-next**:

    ede1618064a9 ("net: devmem: detach TX bindings on NETDEV_UNREGISTER")
    Mina Almasry <almasrymina@google.com>, 2026-10-30, via Paolo Abeni
    Fixes: bd61848900bf ("net: devmem: Implement TX path")
    Fixes: 1abe839b34ae ("net: devmem: support TX over NETMEM_TX_NO_DMA devices")

Two commits that an earlier study named as candidates — `23fe6ac2e97a`
("net: skbuff: make `skb_copy_and_csum_bits()` report unreadable frags")
and `7fec569df355` ("net: skbuff: fail segmentation when a segment can't
be checksummed") — do **not** fix this bug. They are `skb_segment()` /
checksum fixes for the SG-off and checksum-off TX paths (the "zeroes on
the wire" problem that driver patch 0005 of the devmem TX series also
guards against), and they touch neither `net/core/devmem.c` nor
`net/core/netdev-genl.c`.

The upstream patch does not apply to v7.2.15 — too much of
`net/core/devmem.c` has changed since (`NET_DEVMEM_BINDING_LIVE` xarray
marks, `net_devmem_lock_dev()`, `net_devmem_dmabuf_binding_unmap()`,
freelist-based net_iov areas, an existing RX-side
`net_devmem_dev_unregister()`). This is a hand-backport. The patch's own
commit message lists every difference; the important one is that it does
**only the pointer detach**, not upstream's early `dma_buf` unmap, so
the pre-existing DMA-API debug warning "device driver has pending DMA
allocations while released from device" on unload with a live binding
remains. That warning is not a memory-safety bug.

## Where it applies

`v7.2.15`; `git apply --check` also passes on pristine `v7.2.6`.

It is a patch to the kernel itself, so nothing in this repository can
apply it: the guest kernel has to be built with it. It is not under
`ansible/files/patches/`, whose series are for the out-of-tree ionic
modules. The patch has no `Signed-off-by:` line.

## When to drop it

When the guest kernel is one that contains `ede1618064a9`. Remove it
then; it will not apply, since upstream's version of the fix is
structurally different from this backport.

## What was checked

- `git mailinfo` parses the file cleanly (exit 0, empty stderr; author,
  subject and date extracted correctly).
- `git apply --check` passes on pristine v7.2.15 and v7.2.6.
- `net/core/devmem.o` and `net/core/netdev-genl.o` build from the
  patched v7.2.15 sources with the test kernel's `.config`
  (`CONFIG_NET_DEVMEM=y`) and `W=1`, with **no warnings**; the new
  `net_devmem_dev_unregister` symbol and its call site are in the
  objects.
- `checkpatch.pl --strict`: only the deliberate missing `Signed-off-by:`
  and "unknown commit id" warnings (checkpatch ran with `--no-tree`, so
  it cannot resolve any SHA).

**Not run:** the patch has not been booted or runtime-tested; the
reproduction above has not been re-run against a kernel carrying it.
Sparse was not usable on this host. Upstream's own verification was a
netdevsim selftest (`nl_netdev.py`), not real hardware.
