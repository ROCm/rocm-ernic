Architecture
============

rocm-ernic emulates a complete PCIe RDMA device in userspace
using the VFIO-User protocol. A kernel driver inside the guest
VM communicates with the emulated device, providing standard
InfiniBand verbs to applications. That driver is the upstream
Linux ``ionic`` and ``ionic_rdma`` pair, with the patches in
``patches/`` applied. See :doc:`ionic`.

High-Level Overview
-------------------

::

  ┌──────────────────────────────────────────────┐
  │          Virtual Machine (Guest)             │
  │  ┌────────────────────────────────────┐      │
  │  │  Linux ionic + ionic_rdma Drivers  │      │
  │  └─────────────┬──────────────────────┘      │
  │                │ PCI Interface               │
  └────────────────┼─────────────────────────────┘
                   │ VFIO-User Protocol
  ┌────────────────┼─────────────────────────────┐
  │                ▼                             │
  │  ┌──────────────────────────────────────┐    │
  │  │    rocm_ernic Server                 │    │
  │  │  ┌───────────────────────┐           │    │
  │  │  │  RDMA Device Logic    │           │    │
  │  │  │  (adapted from QEMU)  │           │    │
  │  │  └──────────┬────────────┘           │    │
  │  │    ┌────────┴────────┬──────┐        │    │
  │  │    ▼                 ▼      ▼        │    │
  │  │  ┌──────┐  ┌──────────┐  ┌──────┐   │    │
  │  │  │Loop- │  │ TCP/IP   │  │RDMA/ │   │    │
  │  │  │back  │  │ Backend  │  │Verbs │   │    │
  │  │  └──────┘  └────┬─────┘  └──┬───┘   │    │
  │  └─────────────────┼───────────┼────────┘    │
  │                    │           │             │
  │         ┌──────────┘           │             │
  │         ▼                      ▼             │
  │  ┌──────────────┐    ┌──────────────┐        │
  │  │Another       │    │  libibverbs  │        │
  │  │rocm_ernic    │    └──────┬───────┘        │
  │  │Server        │           ▼                │
  │  └──────────────┘    ┌──────────────┐        │
  │                      │ InfiniBand   │        │
  │                      │ Hardware     │        │
  │                      └──────────────┘        │
  └──────────────────────────────────────────────┘

Components
----------

Server (``rocm_ernic_server.c``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The main entry point. It creates the libvfio-user context,
sets up the PCI BARs, registers
MSI-X vectors, and enters the server loop waiting for a QEMU
client to connect.

Compatibility Bridge (``rocm_ernic_compat.c``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

An isolation layer between the QEMU-derived RDMA device logic
and the libvfio-user transport. The bridge exposes a clean
C API (``pvrdma_device_create``, ``pvrdma_regs_write``, etc.)
so that the server never includes QEMU headers directly.

RDMA Device Logic (``third-party/qemu/``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Vendored from the QEMU PVRDMA device implementation. This code
handles the command ring, doorbell processing, queue-pair
management, and completion-queue posting. It is intentionally
kept close to the upstream QEMU source to simplify future
synchronization.

QEMU Compatibility Layer (``src/qemu-compat/``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The QEMU headers and functions the vendored code expects
(``qemu/thread.h``, ``hw/pci/pci.h``, error reporting, and so
on), implemented for a standalone process rather than taken
from QEMU.

RDMA Backends (``src/rdma/``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The backend vtable and its none, loopback and TCP mesh
implementations, described under `Backends`_ below.

Network Services (``src/net/``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The DHCP server, the RDMA CM wire protocol, and Ethernet frame
injection into the emulated NIC's receive path.

ionic Emulation (``src/ionic_*.c``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The device front end. It implements the
register and queue protocol of the AMD
Pensando ionic NIC so the guest can run the upstream Linux
``ionic`` and ``ionic_rdma`` drivers. It provides the device
command interface and the Ethernet LIF
(``ionic_eth_emu.c``), a TAP host backend
(``ionic_eth_net.c``), the admin queue (``ionic_adminq.c``),
and the RDMA device commands (``ionic_rdma_devcmd.c``). The
RDMA operations themselves are handed to the backends below.
See :doc:`ionic`.

PCI BARs
--------

.. list-table::
   :header-rows: 1
   :widths: 10 25 65

   * - BAR
     - Size
     - Purpose
   * - BAR 0
     - 64 KB
     - 32 KB device register window (device info, device
       command, interrupt control) plus the MSI-X table and
       Pending Bit Array above it
   * - BAR 2
     - 4 MB
     - Doorbell pages; the offset within a page encodes the
       queue type, the page index the process ID

Backends
--------

Loopback
^^^^^^^^

In-memory emulation with no external dependencies. Send and
receive operations complete immediately within the same
process. Ideal for driver development and CI testing.

TCP/IP
^^^^^^

Connects two or more rocm-ernic server instances over TCP
in a manager/worker mesh.  The protocol serializes RDMA work
requests, data payloads, and completions across the network.
Instance 1 acts as the mesh manager; instances 2..N connect
as workers.

The TCP backend also forwards raw Ethernet frames between
nodes, enabling IP connectivity (ping, ARP) over the
emulated NIC.  Frames that the server's Ethernet handler
does not process locally (ARP for non-server IPs, ICMP,
arbitrary IP traffic) are broadcast to all mesh peers via
``TCP_MSG_ETH_FRAME`` messages.  The manager acts as a hub,
re-broadcasting frames from one worker to all others.

Userspace Data Path
"""""""""""""""""""

On kernels 6.14 and later, the write-based uverbs handlers
for ``POST_SEND``, ``POST_RECV``, and ``POLL_CQ`` are no
longer available.  The rdma-core provider implements these
operations as direct reads and writes to shared-memory ring
buffers:

- **post_send / post_recv**: write WQE headers and SGEs
  into the mmap'd QP ring buffer, advance the producer
  tail index, and ring the UAR doorbell (a BAR2 write
  trapped by libvfio-user).

- **poll_cq**: read CQE entries from the mmap'd CQ buffer
  using the shared ring state (producer/consumer atomics
  in the CQ header page).

The QP and CQ buffers are allocated by the provider via
``mmap(MAP_PRIVATE | MAP_ANONYMOUS)`` and pinned by the
kernel driver via ``ib_umem_get``.  The server accesses the
same physical pages through DMA mapping
(``rdma_pci_dma_map``).  Ring state synchronization uses
``_Atomic uint32_t`` on the provider side and
``qatomic_read/set`` on the server side, both backed by
the same shared page.

RDMA / Verbs
^^^^^^^^^^^^^

Forwards operations to a real InfiniBand HCA via libibverbs.
Requires an RDMA-capable NIC on the host (for example,
``mlx5_0``).

NVMe-oF
^^^^^^^

Not a transport but a peer: an NVMe over Fabrics target
running inside the server, answering IB CM and NVMe command
capsules from the guest. It hooks into ``ionic_datapath.c``
rather than this vtable, because that is the path the guest
``ionic_rdma`` driver actually drives. One VM and one server
instance are then a complete fabric. See :doc:`nvmeof`.

None
^^^^

Minimal stubs that accept but do not process work requests.
Suitable for PCI enumeration and basic driver bring-up tests.

TAP (Ethernet only)
^^^^^^^^^^^^^^^^^^^

Not an RDMA backend: ``--tap`` binds the
emulated Ethernet LIF to a host TAP interface, so guest
frames land on a real host netdev and the host stack handles
ARP, ICMP, DHCP, and TCP with no protocol emulation in the
server. The TAP fd is non-blocking and drained from the
server's existing poll loop, because only the thread owning
the vfio-user context may DMA into guest memory.
