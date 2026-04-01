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
   performance

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
