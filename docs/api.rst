API Reference
=============

This page documents the rocm-ernic C API, extracted from
annotated source headers by Doxygen and rendered via Breathe.

Userspace Server API
--------------------

Device Structure
^^^^^^^^^^^^^^^^

.. doxygenfile:: rocm_ernic_internal.h

Compatibility Bridge
^^^^^^^^^^^^^^^^^^^^

.. doxygenfile:: rocm_ernic_compat.h

ionic Emulation
---------------

Ethernet Emulator
^^^^^^^^^^^^^^^^^

.. doxygenfile:: ionic_eth_emu.h

RDMA devcmd Handler
^^^^^^^^^^^^^^^^^^^

.. doxygenfile:: ionic_rdma_devcmd.h

Admin Queue
^^^^^^^^^^^

.. doxygenfile:: ionic_adminq.h

Datapath
^^^^^^^^

.. doxygenfile:: ionic_datapath.h
