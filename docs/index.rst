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
- Three memory-mapped BARs (MSI-X, registers, UAR)
- MSI-X interrupt support
- Multiple RDMA backends (loopback, TCP/IP, native verbs)
- Companion Linux kernel driver (``rocm_ernic``)
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

.. toctree::
   :maxdepth: 2
   :caption: User Guide

   building
   architecture
   usage
   service
   driver
   testing

.. toctree::
   :maxdepth: 2
   :caption: API Reference

   api

License
-------

The userspace server and build infrastructure are licensed under
GPL-2.0-or-later. The kernel driver carries the original VMware
dual-license (GPL-2.0 / BSD-2-Clause) for files derived from
the upstream PVRDMA driver.
