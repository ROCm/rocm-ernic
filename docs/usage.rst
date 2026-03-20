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

During development you often need to rebuild the server and
test changes against a running VM. The hot-reload workflow
lets you do this without tearing down and rebooting the VM,
cutting the typical iteration cycle from 60+ seconds down
to roughly 5--15 seconds (depending on build time).

The mechanism uses QEMU's QMP (QEMU Machine Protocol) to
hot-unplug the ``vfio-user-pci`` device, restart the server,
and hot-plug a fresh device -- all while the VM keeps
running.

**Host dependency:** ``socat`` is required for QMP
communication.

.. code-block:: bash

   sudo apt install socat

Starting the VM with hot-reload support
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Use the ``run-vm-vfio-user.sh`` script. It automatically
adds a QMP socket and a PCIe root port so the device can be
hot-plugged:

.. code-block:: bash

   # Start the server
   sudo ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend loopback &

   # Launch the VM (includes QMP + hot-plug support)
   ./scripts/run-vm-vfio-user.sh

The QMP socket defaults to ``/tmp/qemu-qmp.sock`` and can
be overridden with the ``QMP_SOCKET`` environment variable.

Running the hot-reload cycle
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

After the VM is booted and the guest driver is loaded for
the first time, subsequent server changes can be applied
with a single command:

.. code-block:: bash

   ./scripts/hot-reload.sh

This script performs the following steps:

1. Rebuilds the server (``cmake --build build``)
2. Unloads the guest kernel driver via SSH
3. Hot-unplugs the device via QMP ``device_del``
4. Stops the old server process
5. Starts the new server on the same socket
6. Hot-plugs a fresh device via QMP ``device_add``
7. Reloads the guest kernel driver via SSH

Useful options:

- ``--no-build`` -- skip the rebuild step (e.g. when only
  restarting the server)
- ``--build-only`` -- rebuild without cycling the device
- ``--update-driver`` -- also rebuild and reload the guest
  kernel module (copies source via SCP, runs ``make`` and
  ``insmod`` inside the guest)
- ``--backend TYPE`` -- choose a different backend
  (default: ``loopback``)

The script connects to the guest via SSH. The following
environment variables control the connection and can be
set to match your VM image:

.. list-table::
   :header-rows: 1
   :widths: 25 20 55

   * - Variable
     - Default
     - Description
   * - ``SSH_USER``
     - ``stebates``
     - Username for SSH into the guest
   * - ``SSH_PORT``
     - ``2222``
     - Host port forwarded to guest port 22
   * - ``QMP_SOCKET``
     - ``/tmp/qemu-qmp.sock``
     - Path to the QEMU QMP Unix socket
   * - ``VFIO_USER_SOCKET``
     - ``/tmp/vfio-user-rocm-ernic.sock``
     - Path to the vfio-user server socket
   * - ``BACKEND``
     - ``loopback``
     - Server backend type

For example, to use a different SSH user:

.. code-block:: bash

   SSH_USER=ubuntu ./scripts/hot-reload.sh
