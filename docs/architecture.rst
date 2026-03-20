Architecture
============

rocm-ernic emulates a complete PCIe RDMA device in userspace
using the VFIO-User protocol. A companion kernel driver inside
the guest VM communicates with the emulated device, providing
standard InfiniBand verbs to applications.

High-Level Overview
-------------------

::

  ┌──────────────────────────────────────────────┐
  │          Virtual Machine (Guest)             │
  │  ┌────────────────────────────────────┐      │
  │  │   Linux Kernel rocm_ernic Driver   │      │
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
sets up the three PCI BARs, registers MSI-X vectors, and
enters the server loop waiting for a QEMU client to connect.

Compatibility Bridge (``rocm_ernic_compat.c``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

An isolation layer between the QEMU-derived RDMA device logic
and the libvfio-user transport. The bridge exposes a clean
C API (``pvrdma_device_create``, ``pvrdma_regs_write``, etc.)
so that the server never includes QEMU headers directly.

RDMA Device Logic (``src/from-qemu/``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Adapted from the QEMU PVRDMA device implementation. This code
handles the command ring, doorbell processing, queue-pair
management, and completion-queue posting. It is intentionally
kept close to the upstream QEMU source to simplify future
synchronization.

PCI BARs
--------

.. list-table::
   :header-rows: 1
   :widths: 10 25 65

   * - BAR
     - Size
     - Purpose
   * - BAR 0
     - 16 KB
     - MSI-X table and Pending Bit Array (PBA)
   * - BAR 1
     - 256 B (64 DWORDs)
     - Device registers (version, DSR, control,
       interrupt cause/mask, MAC address, Ethernet)
   * - BAR 2
     - 2 MB (512 x 4 KB pages)
     - User Access Region (UAR) doorbells

Backends
--------

Loopback
^^^^^^^^

In-memory emulation with no external dependencies. Send and
receive operations complete immediately within the same
process. Ideal for driver development and CI testing.

TCP/IP
^^^^^^

Connects two rocm-ernic server instances over a TCP socket.
The protocol serializes RDMA work requests and completions
across the network. Useful for multi-VM testing without
RDMA hardware.

RDMA / Verbs
^^^^^^^^^^^^^

Forwards operations to a real InfiniBand HCA via libibverbs.
Requires an RDMA-capable NIC on the host (for example,
``mlx5_0``).

None
^^^^

Minimal stubs that accept but do not process work requests.
Suitable for PCI enumeration and basic driver bring-up tests.
