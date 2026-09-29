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
- An AMD Pensando ionic NIC, driven in the guest by the
  upstream Linux ``ionic`` and ``ionic_rdma`` drivers
  (see :doc:`ionic`)
- Memory-mapped BARs (MSI-X, registers, doorbells)
- MSI-X interrupt support
- Multiple RDMA backends (loopback, TCP/IP, native verbs)
- An in-process NVMe-oF target, so one VM and one server
  are a complete fabric (see :doc:`nvmeof`)
- An in-process S3-over-RDMA object store, with its own
  in-band HTTP endpoint on the emulated wire, so the same
  single VM is a complete object fabric (see :doc:`s3`)
- Working Ethernet and TCP/IP to the host via a TAP
  interface
- Comprehensive statistics collection

Quick Start
^^^^^^^^^^^

.. code-block:: bash

   sudo apt install cmake meson ninja-build pkg-config \
     libibverbs-dev librdmacm-dev libglib2.0-dev libjson-c-dev
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
   nvmeof
   s3
   service
   monitoring
   testing
   performance
   nvmeof-performance
   perf-trends

.. toctree::
   :maxdepth: 2
   :caption: API Reference

   api

Acknowledgments
---------------

The RDMA device logic in ``third-party/qemu/`` is adapted from the
QEMU PVRDMA implementation. The original authors of that work:

- Yuval Shaia <yuval.shaia@oracle.com> (Oracle)
- Marcel Apfelbaum <marcel@redhat.com> (Red Hat)

License
-------

The build system, documentation, and the deployment and automation code are licensed
under the
`MIT license <https://github.com/ROCm/rocm-ernic/blob/develop/LICENSE.md>`_.
The emulator itself is ``GPL-2.0-or-later``:

- Everything under ``src/``, ``tests/`` and ``third-party/``, and the fuzz
  harnesses in ``nix/analysis/fuzz/``, which build against ``src/``.
- The VMware/Linux uAPI headers under
  ``third-party/qemu/include/standard-headers/`` are instead
  dual ``GPL-2.0`` / ``BSD-2-Clause``.

The groupings above are a summary; the per-file ``SPDX-License-Identifier``
notice is authoritative. See
`LICENSE.md <https://github.com/ROCm/rocm-ernic/blob/develop/LICENSE.md>`_
for the full statement.
