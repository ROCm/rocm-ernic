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

Ethernet Registers (Userspace)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. doxygenfile:: src/rocm_ernic_eth.h

Kernel Driver API
-----------------

Device API
^^^^^^^^^^

.. doxygenfile:: rocm_ernic_dev_api.h

ABI Definitions
^^^^^^^^^^^^^^^

.. doxygenfile:: rocm_ernic-abi.h

Verbs Structures
^^^^^^^^^^^^^^^^

.. doxygenfile:: rocm_ernic_verbs.h

PCI Identifiers
^^^^^^^^^^^^^^^

.. doxygenfile:: rocm_ernic_pci_ids.h
