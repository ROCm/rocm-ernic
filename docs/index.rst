rocm-ernic: Emulated RDMA NIC for Virtual Machines
===================================================

Introduction
------------

rocm-ernic is a userspace RDMA device server built on the
`libvfio-user <https://github.com/nutanix/libvfio-user>`_
framework. It provides full RDMA (Remote Direct Memory Access)
functionality to virtual machines without requiring physical
RDMA hardware or an in-guest software stack such as
`Soft-RoCE <https://man7.org/linux/man-pages/man7/rxe.7.html>`_.

Key Features
^^^^^^^^^^^^

- Full PCIe device emulation in userspace
- Two device personalities: the default ionic mode, driven by
  the upstream Linux ``ionic`` driver, and a deprecated
  PVRDMA-derived device driven by the companion
  ``rocm_ernic`` module (see :doc:`ionic`)
- Memory-mapped BARs (MSI-X, registers, doorbells)
- MSI-X interrupt support
- Multiple RDMA backends (loopback, TCP/IP, native verbs)
- Working Ethernet and TCP/IP to the host via a TAP
  interface in ionic mode
- Comprehensive statistics collection

Quick Start
^^^^^^^^^^^

.. code-block:: bash

   sudo apt install cmake meson ninja-build pkg-config \
     libibverbs-dev librdmacm-dev libglib2.0-dev
   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
   cmake --build build

   ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend loopback --verbose

That presents the device to the guest as an upstream-driven
ionic NIC. Add ``--tap`` to attach its Ethernet interface to
a host TAP:

.. code-block:: bash

   ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend loopback --tap ernic0

.. toctree::
   :maxdepth: 2
   :caption: User Guide

   building
   architecture
   usage
   ionic
   service
   monitoring
   driver
   testing
   performance
   perf-trends

.. toctree::
   :maxdepth: 2
   :caption: API Reference

   api

Acknowledgments
---------------

The RDMA device logic in ``src/from-qemu/`` is adapted from the
QEMU PVRDMA implementation. The original authors of that work:

- Yuval Shaia <yuval.shaia@oracle.com> (Oracle)
- Marcel Apfelbaum <marcel@redhat.com> (Red Hat)

License
-------

The project is licensed under the
`MIT license <https://github.com/ROCm/rocm-ernic/blob/main/LICENSE.md>`_.
Some files carry different licenses per their SPDX headers:

- Files under ``src/from-qemu/`` are derived from QEMU and are
  licensed under ``GPL-2.0-or-later``.
- Files under ``driver/`` are Linux kernel driver sources and
  carry ``GPL-2.0 / BSD-2-Clause`` dual licenses as indicated
  by their SPDX headers.
