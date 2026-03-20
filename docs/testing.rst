Testing
=======

rocm-ernic ships with several test programs and a CTest
integration that can be run from the build directory.

Test Programs
-------------

test_pci_client
^^^^^^^^^^^^^^^

A vfio-user client that connects to the server and performs
basic PCI configuration space queries:

- Socket connection to server
- PCI Vendor ID verification (AMD: ``0x1022``)
- PCI Device ID verification (ROCm ERNIC: ``0x8000``)
- PCI Class Code verification (Network Controller)
- PCI Header Type verification (Type 0)
- BAR register reads
- Interrupt configuration reads

Exit codes: ``0`` = pass, ``1`` = failure.

test_data_transfer
^^^^^^^^^^^^^^^^^^

Comprehensive RDMA data transfer test using libibverbs:

- RDMA device discovery and opening
- Protection Domain allocation
- Completion Queue creation
- Queue Pair creation and state transitions
- Memory Region registration
- Send / recv operations with varying buffer sizes
  (64 to 4096 bytes)

Requires an RDMA device (via the ``rocm_ernic`` driver or
real hardware). Skipped if no device is found.

test_rdma_cm
^^^^^^^^^^^^

RDMA Connection Manager test using libibverbs. Validates
connection setup and teardown paths.

Running Tests
-------------

Quick Local Test
^^^^^^^^^^^^^^^^

.. code-block:: bash

   ./scripts/run-local-tests.sh

This script builds the project (if needed), starts the
server, runs the test client, and cleans up automatically.

Manual Testing
^^^^^^^^^^^^^^

Build and start the server in one terminal:

.. code-block:: bash

   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
   cmake --build build

   ./build/rocm-ernic /tmp/test.sock

Run the test client in another terminal:

.. code-block:: bash

   ./build/tests/test_pci_client --socket /tmp/test.sock

CTest
^^^^^

Run all registered tests via CTest:

.. code-block:: bash

   ctest --test-dir build

With verbose output on failure:

.. code-block:: bash

   ctest --test-dir build --output-on-failure

Adding New Tests
----------------

1. Create a test source file in ``tests/``.
2. Add the executable to ``tests/CMakeLists.txt``.
3. Register the test with ``add_test()``.

Example:

.. code-block:: cmake

   add_executable(test_new_feature
       test_new_feature.c
   )

   add_test(
       NAME new-feature-test
       COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run-test.sh
           $<TARGET_FILE:test_new_feature>
           $<TARGET_FILE:rocm-ernic>
   )
   set_tests_properties(new-feature-test PROPERTIES
       TIMEOUT 30
       RUN_SERIAL TRUE
   )
