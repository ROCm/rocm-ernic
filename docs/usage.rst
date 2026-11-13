Usage
=====

Basic Usage
-----------

Start the server with a UNIX socket and the desired backend:

.. code-block:: bash

   # Loopback backend (no external dependencies)
   ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend loopback

   # In-process NVMe-oF controller the guest can connect to
   ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend nvmeof:size=1G,bs=4096 --tap ernic0

   # No backend (minimal stubs)
   ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend none

   # Ethernet attached to a host TAP
   ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend loopback --tap ernic0

``rocm-ernic --help`` lists every option and each backend's
settings. Those that shape the NIC (``--nic-profile``,
``--print-profile``, ``--eth-queues``,
``--stats-interval-ms``) and ``--inject`` are described in
:doc:`ionic`, and the P4 engine's (``--p4``,
``--p4-program``, ``--p4-guest-scope``) in :doc:`p4`.

Whoever connects to the socket drives the device as the
guest's QEMU, so the server makes it mode ``0600``: only the
user the server runs as, and root, can connect. Run QEMU as
the same user as the server, or as root. To let a QEMU that
runs as another user connect, name a group it is in with
``--socket-group`` (``-G``); the server gives the socket that
group and makes it ``0660`` before it serves it. The server
must run as root or be a member of the group:

.. code-block:: bash

   sudo ./build/rocm-ernic \
     --socket /run/vfio-user-rocm-ernic.sock \
     --socket-group kvm --backend loopback

Changing the socket's group by hand after the server starts
works too, but the next server started on that path, after a
restart or a hot reload, creates a new socket without it.

The server refuses to start if the socket ends up with
different permissions, which a default ACL on its directory
can cause, or with an ACL of its own, which it inherits from a
default ACL with more than the owner, group, and other
entries. An entry naming another user would let that user
connect without the mode showing it. Remove the default ACL
(``setfacl -k DIR``), or put the socket in another directory.

Put the socket in a directory that only the server's user and
root can write to, such as ``/run/rocm-ernic``, which the
service uses, or one of your own. Don't put it in a directory
other users can write to, such as ``/tmp``, even though the
server's default socket and the examples here are there. The
sticky bit on ``/tmp`` stops other users removing or replacing
the server's socket, but not creating their own at the same
name first, while the server isn't running. A server that
isn't root then can't remove it and fails to start, and a QEMU
that connects to it gives that user the guest's memory, which
vfio-user shares with whatever serves the device. In a
directory others can write to that isn't sticky, they can also
replace the server's socket.

With ``--socket-group``, the group also has to be able to reach
the socket through its directory, which ``/run/rocm-ernic``
allows (mode ``0755``) and ``$XDG_RUNTIME_DIR`` (``0700``)
doesn't. A directory of your own can have that group and mode
``0710``, which lets the group reach the socket but not list or
change the directory:

.. code-block:: bash

   sudo install -d -o root -g kvm -m 0710 /run/vfio-user

The Emulated Device
-------------------

The server emulates an AMD Pensando ionic NIC
(``1dd8:1002``) with subsystem ``1dd8:5400``, driven by the
upstream Linux ``ionic`` and ``ionic_rdma`` modules. Once the
``1dd8:5400`` entry is published in ``pci.ids``, ``lspci``
names that subsystem ``ROCm Emulated RDMA NIC`` after
``update-pciids``; until then it displays ``Device [1dd8:5400]``.
There is nothing to select.

``--tap IFNAME`` (short ``-T``) attaches the emulated
Ethernet interface to an existing host TAP. ``IFNAME`` has at
most 15 characters, the Linux limit; the server refuses a
longer one rather than shortening it. Create the TAP up
front, owned by the user running the server:

.. code-block:: bash

   sudo ip tuntap add dev ernic0 mode tap user "$USER"

The two paths are independent: ``--backend`` governs the RDMA
data path, ``--tap`` the Ethernet one. See :doc:`ionic` for
the full picture.

``--mac XX:XX:XX:XX:XX:XX`` (short ``-m``) sets the MAC
address the device reports. Without it every server's guest
sees ``02:a0:d1:00:00:01``, so give each server on a shared
segment its own, as the service does (see :doc:`service`).
The guest can change it, as on any NIC, until the next
reset.

``--cmb`` gives the device an 8 MB Controller Memory Buffer
(CMB), BAR 4, which the guest's RDMA driver may put queues
in. It is off by default, so every queue is in guest memory.
With it, rdma-core puts a QP's queues in the CMB unless the
application opts out with ``ionic_dv_pd_set_sqcmb()`` and
``ionic_dv_pd_set_rqcmb()``, as rocSHMEM does. Writing to a
queue in the CMB costs more in emulation than on hardware:
each store of up to 8 bytes into BAR 4 exits to QEMU and
becomes a message to the server, where a queue in guest
memory is written directly.

``nvmeof`` is the odd one out: rather than carrying guest
RDMA traffic somewhere, it answers it, presenting an NVMe
over Fabrics target the guest connects to with stock
``nvme connect -t rdma``. See :doc:`nvmeof`. ``s3`` is
similar, with an object store in place of the controller;
see :doc:`s3`.

Connecting Several VMs
----------------------

One server serves one VM. For guests in different VMs to
reach each other over RDMA, run each server with the ``tcp``
backend, which joins the servers in a mesh over TCP. One
server is the manager, and the rest are workers that connect
to it:

.. code-block:: bash

   # The manager, listening on port 5000
   ERNIC_TCP_GUEST_GIDS=fe80::706f:63ff:fe6d:2d01 \
     ./build/rocm-ernic --socket /tmp/ernic-1.sock \
     --backend tcp:manager:listen:5000 \
     --mac 72:6f:63:6d:2d:01 --tap ernic1

   # A worker, connecting to the manager
   ERNIC_TCP_GUEST_GIDS=fe80::706f:63ff:fe6d:2d02 \
     ./build/rocm-ernic --socket /tmp/ernic-2.sock \
     --backend tcp:worker:192.168.1.100:5000 \
     --mac 72:6f:63:6d:2d:02 --tap ernic2

A host is a name, or an IPv4 or IPv6 address; an IPv6
address goes in brackets, as in
``tcp:worker:[fe80::1%eth0]:5000``.

Each server must be told its guest's RoCE addresses (GIDs)
in ``ERNIC_TCP_GUEST_GIDS``, since nothing else tells it
which addresses its guest will use. A destination no server
claims is unreachable: connecting a QP to it fails. The
systemd service sets all of this up, deriving each
instance's link-local GID from its MAC (see :doc:`service`).
:doc:`architecture` describes how the mesh works.

Environment Variables
---------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Variable
     - Effect
   * - ``ERNIC_LOG_LEVEL``
     - The log level, as ``--log-level`` (see `Log Levels`_),
       which wins over it
   * - ``ERNIC_TCP_GUEST_GIDS``
     - ``tcp`` only: up to eight comma-separated RoCE
       addresses of this server's guest, each an IPv6 or IPv4
       address. A malformed one is logged and skipped
   * - ``ERNIC_TCP_HEALTH_INTERVAL``
     - ``tcp`` only: seconds between the manager's
       heartbeats (default 5). A worker that hears nothing
       from the manager for three intervals connects again
   * - ``ERNIC_TCP_SEND_TIMEOUT_MS``
     - ``tcp`` only: how long a send to a node that has
       stopped reading may make no progress before the
       connection is closed (default 10000; the manager uses
       half)
   * - ``ERNIC_TCP_SOCKBUF``
     - ``tcp`` only: a fixed size, in bytes, for each mesh
       socket's send and receive buffers. Unset, the kernel
       sizes them itself, which is usually faster, since a
       fixed size is capped at ``net.core.wmem_max`` and
       ``rmem_max``
   * - ``ERNIC_TCP_LISTEN_BACKLOG``
     - ``tcp`` only: the listening socket's backlog
       (default 32)

A numeric variable whose value isn't a positive integer is
ignored, and its default used.

Log Levels
----------

``--log-level`` (short ``-L``) sets how much the server
prints.  It can also be set with the ``ERNIC_LOG_LEVEL``
environment variable; the command line wins when both are
given.

.. list-table::
   :header-rows: 1
   :widths: 15 85

   * - Level
     - Output
   * - ``none``
     - Nothing, unless the server aborts on a bug, which it
       reports in a ``FATAL:`` line.
   * - ``error``
     - ``ERROR:`` lines only.
   * - ``warn``
     - **Default.** ``ERROR:`` and ``WARN:`` lines, plus a
       short startup/shutdown banner.
   * - ``info``
     - Adds the per-operation ``INFO:`` lines (BAR writes,
       QP operations, ARP/ICMP packets, TCP mesh events).
   * - ``debug``
     - Everything, including libvfio-user debug output.

``-v`` / ``--verbose`` remains as shorthand for
``--log-level debug``.

``ERROR:`` and ``WARN:`` lines go to stderr, and the rest to
stdout; ``--log-file`` sends both to one file.  Each line is
written whole, so lines from different threads never run into
each other.  Every message goes through the log, so the level
applies to all of them, errors included.  The command line is
checked before ``--log-level`` takes effect, so only
``ERNIC_LOG_LEVEL`` silences an error about an option.

.. code-block:: bash

   # Quiet by default -- only warnings and errors
   ./build/rocm-ernic --backend loopback

   # Full per-operation tracing
   ERNIC_LOG_LEVEL=info ./build/rocm-ernic --backend loopback

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

Inside the guest, load the upstream modules and verify:

.. code-block:: bash

   sudo modprobe ionic
   sudo modprobe ionic_rdma
   sudo update-pciids
   lspci -nnv -d 1dd8:1002 | grep '\[1dd8:5400\]'
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

- The instance: its socket, backend, PCI ID, MAC, client
  connection state, and NIC profile
- Device-level statistics (commands, BAR 0 and doorbell
  reads and writes, interrupts, resets, and bytes moved by
  RDMA and over the Ethernet side)
- The Ethernet LIF and port counters (see :doc:`ionic`)
- Per-QP statistics:

  - Doorbell rings (send and receive)
  - WQEs processed (total and by opcode type)
  - CQEs posted
  - Bytes sent, received, read, and written

:doc:`monitoring` describes how ``ernic-exporter`` publishes
them.

Statistics are automatically written on server exit
(``SIGINT`` / ``SIGTERM``) in addition to the periodic
updates.

Each update is written to a temporary file next to the stats
file (its name with six random characters appended), which
then replaces the stats file, so a reader always sees one
whole update. The server therefore needs write permission on
the stats file's directory. The file is mode ``0644``
whatever the umask. If an update can't be written, the server
logs why and leaves the previous file in place.

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

It logs in to the guest at ``localhost`` port
``SSH_PORT`` (default ``2222``) as ``SSH_USER``
(default: the user running the script).
