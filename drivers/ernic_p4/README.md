# ernic_p4 — a guest control driver for the P4 pipeline

A small kernel module that binds to the `ionic.p4` auxiliary device and
puts `/dev/ernic-p4` in front of the emulated NIC's P4 control surface.
Through it a program in the guest can load a compiled P4 program, install
table entries, and read the engine's counters.

Unlike everything in [`patches/`](../../ansible/files/patches/), this is **our** module,
not a modification of an upstream one, so it lives here as source rather
than as a patch against someone else's tree.

## What it is and is not

It is a pipe. Each ioctl becomes exactly one ionic admin-queue command —
70 (identify), 71 (RPC), 72 (query) — and the driver does not interpret
the payloads. The payload formats belong to the device and are documented
in [`docs/p4.rst`](../../docs/p4.rst).

It is not a policy. What a guest may do is the **operator's** decision,
made on the host with `--p4-guest-scope` and enforced by the device. This
driver neither checks nor second-guesses that: a refusal comes back as the
device's own status code, which the ioctl passes up unchanged rather than
flattening into an errno, so a caller can tell "not permitted" from
"malformed" and from "no such operation".

What it does own is the boundary between a userspace pointer and a device
DMA address. Buffers are bounced through coherent memory the driver
allocates, never by pinning the caller's pages, so a page the caller
unmaps between the ioctl and the completion is not something the device
can still reach. The bounce buffers are cleared before each use, so a
short request cannot leave the previous caller's bytes where the device
reads them as this one's, and the length the device reports having written
is clamped to the buffer the caller actually offered.

## Building it

The module uses the ionic driver's private headers — `ionic_api.h` and
what it pulls in — which no `linux-headers` package ships, because they
live in the driver's own directory in the kernel tree. So it builds
against a prepared ionic source tree, not just against kernel headers:

```bash
ansible/files/scripts/fetch-ionic-sources.sh \
    --source-dir /tmp/ionic-src --kernel-ref v7.2.4 \
    --patches-dir ansible/files/patches \
    --patches-dir ansible/files/patches/p4

make -C drivers/ernic_p4 \
    KDIR=/lib/modules/$(uname -r)/build \
    IONIC_SRC=/tmp/ionic-src
```

Both series are required. The base series is what the rest of the stack
expects, and [`patches/p4`](../../ansible/files/patches/p4/) is what defines
`IONIC_LIF_CAP_P4` and creates the `ionic.p4` device this binds to. The
`Makefile` checks for `IONIC_LIF_CAP_P4` in the tree it is given and
refuses rather than building a module that could never bind — a module
that loads and silently never probes is a much harder thing to diagnose
than a build that stops.

## Installing it

The `ernic_p4_dkms` Ansible role registers the module with DKMS rather
than copying a built `.ko` into `/lib/modules`, because it has to be
rebuilt whenever the kernel changes: it is pinned to the genksyms CRCs
of an out-of-tree ionic that is itself DKMS-built and rebuilt on the
same upgrade. A module left behind by a hand-installed build survives
the upgrade and then will not insert.

[`dkms.conf`](dkms.conf) therefore has to find both inputs again from a
context that has none of the role's variables. It reads the source tree
from `/etc/ernic-p4-ionic-src`, which the role writes, and resolves the
ionic symbols through
[`find-ionic-symvers.sh`](find-ionic-symvers.sh) **for the kernel being
built for** -- not for whatever is on disk, which after an upgrade
belongs to the previous one. Either can be overridden by setting
`IONIC_SRC` or `IONIC_SYMVERS` in the environment, which is what the
role does for the build it drives itself.

## When the device appears

Only when the NIC advertises `IONIC_LIF_CAP_P4`, which rocm-ernic does
only when the P4 engine is enabled **and** the operator granted the guest
a scope. So:

| Server | `ionic.p4` device | `/dev/ernic-p4` |
| --- | --- | --- |
| no `--p4` | absent | absent |
| `--p4`, no `--p4-guest-scope` | absent | absent |
| `--p4 --p4-guest-scope read` | present | present, read-only operations succeed |
| `--p4-guest-scope load` | present | present, program load succeeds |

A host running more than one emulated NIC gets `/dev/ernic-p4` for the
first and `/dev/ernic-p4.N` for the rest, so a second probe does not
fail on a name already taken.

Those numbers are this driver's, not the auxiliary bus's. ionic
allocates aux ids from one allocator shared with its `rdma` device, so
the first P4 device on a host is `ionic.p4.1`; naming the node after
that id left a machine with exactly one NIC offering only
`/dev/ernic-p4.1`, which is not what anything looks for.

The node is mode 0600. The engine decides where every frame goes, so it
is not something to open to a group by default.

## Interface

[`uapi/ernic_p4.h`](uapi/ernic_p4.h) is the userspace header, carrying the
three ioctls and their argument structures.
