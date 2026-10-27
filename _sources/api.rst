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

Resource Manager
^^^^^^^^^^^^^^^^

.. doxygenfile:: ernic_rm.h

Mesh
^^^^

.. doxygenfile:: ernic_mesh.h

Statistics
^^^^^^^^^^

.. doxygenfile:: ernic_stats.h

ionic Emulation
---------------

BAR Layout
^^^^^^^^^^

.. doxygenfile:: ionic_bar.h

Device
^^^^^^

.. doxygenfile:: ionic_device.h

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

Datapath Targets
^^^^^^^^^^^^^^^^

.. doxygenfile:: ionic_datapath_nvmeof.h

.. doxygenfile:: ionic_datapath_s3.h

P4 Attachment
^^^^^^^^^^^^^

Built only with ``ERNIC_BUILD_P4``.

.. doxygenfile:: ionic_datapath_p4.h

.. doxygenfile:: ionic_eth_p4.h
