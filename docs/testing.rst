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

Multi-VM RDMA Testing
---------------------

With two VMs launched via ``ernicctl``, you can run
standard RDMA benchmarks over the emulated NICs.

Prerequisites:

1. Start the rocm-ernic service and launch two VMs
   (see :doc:`service`).
2. Install the driver and custom rdma-core v62 in
   both VMs (see ``ernicctl driver-push``).
3. Configure IP addresses on the rocm-ernic NICs
   (``enp1s0``) in both VMs.

ibv_rc_pingpong
^^^^^^^^^^^^^^^

Latency test using RC (Reliable Connection) QPs:

.. code-block:: bash

   # VM 1 (server):
   LD_LIBRARY_PATH=/opt/rdma-core-ernic/lib \
     ibv_rc_pingpong -d rocep1s0 -g 1 -n 100

   # VM 2 (client, use multicast NIC for OOB):
   LD_LIBRARY_PATH=/opt/rdma-core-ernic/lib \
     ibv_rc_pingpong -d rocep1s0 -g 1 -n 100 \
     192.168.100.10

Expected output (TCP mesh backend):

::

   40960 bytes in 0.37 seconds = 0.89 Mbit/sec
   5 iters in 0.37 seconds = 73960.40 usec/iter

ib_send_bw
^^^^^^^^^^

Bandwidth test using the perftest suite:

.. code-block:: bash

   # VM 1 (server):
   LD_LIBRARY_PATH=/opt/rdma-core-ernic/lib \
     ib_send_bw -d rocep1s0 -x 1 -n 10 \
     --report_gbits

   # VM 2 (client):
   LD_LIBRARY_PATH=/opt/rdma-core-ernic/lib \
     ib_send_bw -d rocep1s0 -x 1 -n 10 \
     --report_gbits 192.168.100.10

Expected output:

::

   #bytes  #iterations  BW peak[Gb/sec]  BW average[Gb/sec]
   65536   10           0.79             0.12

Ethernet Connectivity
^^^^^^^^^^^^^^^^^^^^^

The emulated NICs support IP over Ethernet via frame
forwarding through the TCP mesh.  Ping between VMs:

.. code-block:: bash

   # VM 1:
   sudo ip link set enp1s0 up
   sudo ip addr add 192.168.200.10/24 dev enp1s0

   # VM 2:
   sudo ip link set enp1s0 up
   sudo ip addr add 192.168.200.20/24 dev enp1s0

   # From VM 1:
   ping 192.168.200.20

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
