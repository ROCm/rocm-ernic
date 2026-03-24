Usage
=====

Basic Usage
-----------

Start the server with a UNIX socket and the desired backend:

.. code-block:: bash

   # Loopback backend (no external dependencies)
   ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend loopback \
     --verbose

   # RDMA verbs backend (requires RDMA hardware)
   ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend verbs:device=mlx5_0,ethdev=eth0,port=1 \
     --verbose

   # No backend (minimal stubs)
   ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend none

Launching a VM
--------------

Attach the emulated device to a QEMU virtual machine using
the ``vfio-user-pci`` transport. QEMU 10.1 or later is
required.

.. code-block:: bash

   qemu-system-x86_64 \
     -machine q35,accel=kvm \
     -cpu EPYC \
     -smp cpus=2 \
     -object memory-backend-memfd,\
   id=mem0,share=on,size=2048M \
     -machine memory-backend=mem0 \
     -nographic \
     -drive if=virtio,format=qcow2,\
   file=vm-image.qcow2 \
     -netdev user,id=net0,\
   hostfwd=tcp::2222-:22 \
     -device virtio-net-pci,netdev=net0 \
     -device '{"driver":"vfio-user-pci",\
   "socket":{"path":"/tmp/vfio-user-rocm-ernic.sock",\
   "type":"unix"}}'

Inside the guest, load the kernel driver and verify:

.. code-block:: bash

   sudo modprobe rocm_ernic
   lspci | grep 1022:1488
   ibv_devices

Statistics Collection
---------------------

The server can collect detailed statistics about doorbell
rings, WQE processing, and completion queue entries.
Statistics are written to a file approximately every second
while the server is running.

.. code-block:: bash

   ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend loopback \
     --stats-file /tmp/rocm_ernic_stats.txt

   # Monitor statistics in real-time
   watch -n 0.5 cat /tmp/rocm_ernic_stats.txt

The statistics file includes:

- Device-level statistics (commands, register reads/writes,
  UAR writes, interrupts)
- Per-QP statistics:

  - Doorbell rings (send, receive, SRQ)
  - WQEs processed (total and by opcode type)
  - CQEs posted
  - Continuation callbacks scheduled

Statistics are automatically written on server exit
(``SIGINT`` / ``SIGTERM``) in addition to the periodic
updates.

Development Workflow: Hot Reload
--------------------------------

During development you often need to rebuild the server
and test changes against a running VM.  The hot-reload
workflow lets you do this without tearing down and
rebooting the VM, cutting the typical iteration cycle
from 60+ seconds down to roughly 5--15 seconds
(depending on build time).

The recommended approach is the ``ernicctl`` service
CLI, which manages server instances, VMs, QMP sockets,
and driver distribution through a single tool.  See
:doc:`service` for full documentation.

Quick example using ernicctl
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: bash

   # Start the service (launches all instances)
   sudo systemctl start rocm-ernic

   # Launch a VM for instance 1
   sudo ernicctl vm-launch 1

   # After making code changes, hot-reload instance 1
   sudo ernicctl hot-reload 1 --update-driver

Standalone hot-reload script
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The ``scripts/hot-reload.sh`` script is also available
for standalone use outside the systemd service.  It
uses QEMU's QMP (QEMU Machine Protocol) to hot-unplug
the ``vfio-user-pci`` device, restart the server, and
hot-plug a fresh device -- all while the VM keeps
running.

**Host dependency:** ``socat`` is required for QMP
communication.

.. code-block:: bash

   sudo apt install socat

.. code-block:: bash

   ./scripts/hot-reload.sh

Useful options:

- ``--no-build`` -- skip the rebuild step
- ``--build-only`` -- rebuild without cycling the
  device
- ``--update-driver`` -- rebuild and reload the guest
  kernel module
- ``--backend TYPE`` -- choose a different backend
  (default: ``loopback``)
