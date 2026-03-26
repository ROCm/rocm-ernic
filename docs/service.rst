systemd Service and ernicctl
============================

rocm-ernic ships a systemd service and a companion
``ernicctl`` CLI for managing one or more server
instances, launching VMs, performing live hotplug, and
distributing the kernel driver to guests.

.. contents:: On this page
   :local:
   :depth: 2

Overview
--------

The service replaces the legacy ``setup-rocm-ernic``
script with a production-grade systemd unit.  Key
capabilities:

- Start/stop N server instances in a TCP manager/worker
  mesh with a single ``systemctl`` command.
- Deterministic per-instance MAC addresses derived from
  the host's ``/etc/machine-id``.
- Dynamic mesh scaling -- add or remove worker instances
  at runtime.
- Launch QEMU VMs via qemu-minimal's ``run-vm`` and
  hot-plug/unplug vfio-user devices via QMP.
- Build and push a self-contained driver tarball to
  guest VMs.

Prerequisites
^^^^^^^^^^^^^

- Python 3 (standard library only; no pip packages)
- ``socat`` (for QMP communication)
- QEMU 10.1+ with vfio-user support
- qemu-minimal checkout with ``run-vm`` supporting
  ``QMP_SOCKET`` and ``VFIO_USERDEV``

.. code-block:: bash

   sudo apt install socat python3

Installation
------------

Build and install with CMake:

.. code-block:: bash

   cmake -B build -G Ninja \
     -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_INSTALL_PREFIX=/usr/local \
     -DERNIC_INSTALL_SERVICE=ON
   cmake --build build
   sudo cmake --install build

This installs:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Destination
     - Contents
   * - ``/usr/local/bin/``
     - ``rocm-ernic``, ``ernicctl``
   * - ``/usr/local/libexec/rocm-ernic/``
     - ``rocm-ernic-launcher``,
       ``rocm-ernic-driver-pack``
   * - ``/etc/rocm-ernic/``
     - ``rocm-ernic.env``
   * - ``/usr/local/share/rocm-ernic/``
     - ``vm-driver-install.sh.in``, ``driver/``
   * - ``/usr/lib/systemd/system/``
     - ``rocm-ernic.service``,
       ``rocm-ernic-driver-pack.service``

After installation, reload systemd and optionally
enable the service at boot:

.. code-block:: bash

   sudo systemctl daemon-reload
   sudo systemctl enable rocm-ernic

Configuration
-------------

All settings live in ``/etc/rocm-ernic/rocm-ernic.env``
(a shell-sourceable key=value file used by systemd's
``EnvironmentFile=`` directive).  Edit this file to
match your environment.

Server settings
^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 30 20 50

   * - Variable
     - Default
     - Description
   * - ``ERNIC_BIN``
     - ``/usr/local/bin/rocm-ernic``
     - Path to the server binary
   * - ``ERNIC_RUN_DIR``
     - ``/run/rocm-ernic``
     - Runtime directory (sockets, stats, manifest)
   * - ``ERNIC_LOG_DIR``
     - ``/var/log/rocm-ernic``
     - Log directory
   * - ``ERNIC_INSTANCES``
     - ``4``
     - Total instances (1 manager + N-1 workers)
   * - ``ERNIC_TCP_PORT``
     - ``6320``
     - TCP port for the manager/worker mesh
   * - ``ERNIC_MANAGER_IP``
     - ``127.0.0.1``
     - IP address workers connect to
   * - ``ERNIC_VERBOSE``
     - ``false``
     - Enable verbose server logging

MAC address overrides
^^^^^^^^^^^^^^^^^^^^^

By default, each instance gets a deterministic MAC
derived from ``/etc/machine-id`` and the instance index.
To assign explicit MACs, set ``ERNIC_MAC_1`` through
``ERNIC_MAC_N``:

.. code-block:: bash

   ERNIC_MAC_1=02:aa:bb:cc:00:01
   ERNIC_MAC_2=02:aa:bb:cc:00:02

VM and QEMU settings
^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 30 20 50

   * - Variable
     - Default
     - Description
   * - ``ERNIC_QEMU_MINIMAL``
     - ``$HOME/Projects/qemu-minimal``
     - Path to qemu-minimal checkout
   * - ``ERNIC_QEMU_PATH``
     - ``/opt/qemu-v10.1.2/bin/``
     - Directory containing ``qemu-system-x86_64``
   * - ``ERNIC_VM_IMAGE_DIR``
     - ``$ERNIC_QEMU_MINIMAL/images``
     - Directory with qcow2 VM images
   * - ``ERNIC_VM_NAME``
     - ``stebates-test-vm``
     - Base VM name (qcow2 filename stem)
   * - ``ERNIC_VM_VCPUS``
     - ``4``
     - vCPUs per VM
   * - ``ERNIC_VM_MEM``
     - ``8192``
     - Memory per VM (MB)
   * - ``ERNIC_VM_SSH_USER``
     - ``ubuntu``
     - SSH user for guest access
   * - ``ERNIC_VM_SSH_BASE_PORT``
     - ``2222``
     - Base SSH port (instance N uses base + N - 1)
   * - ``ERNIC_VM_BACKING``
     - (unset)
     - Golden backing qcow2. When set and a
       per-instance overlay does not exist,
       ``ernicctl vm-launch`` creates one
       automatically via ``qemu-img create``.

Driver settings
^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Variable
     - Default
     - Description
   * - ``ERNIC_DRIVER_SOURCE``
     - ``/usr/share/rocm-ernic/driver``
     - Installed driver source tree
   * - ``ERNIC_DRIVER_TARBALL``
     - ``/tmp/rocm-ernic-driver.tar.gz``
     - Path to the built tarball

Using the Service
-----------------

Start, stop, and check status:

.. code-block:: bash

   sudo systemctl start rocm-ernic
   sudo systemctl status rocm-ernic
   sudo systemctl stop rocm-ernic

Or use ``ernicctl`` which wraps ``systemctl``:

.. code-block:: bash

   sudo ernicctl start
   ernicctl status
   sudo ernicctl stop

The ``ernicctl status`` command shows a table of all
instances with their PID, MAC, socket, VM attachment,
IP and RDMA byte counts (TX/RX), liveness state,
uptime, and log file path:

.. code-block:: text

   ID  ROLE      PID    STATE      UPTIME     MAC                SOCKET                         VM           IP (TX/RX)      RDMA (TX/RX)    LOG
   1   manager   12345  running    2h15m      02:a1:b2:c3:d4:01  /run/rocm-ernic/1.sock         67890:2222   1.2K/3.4K       45M/12M         /var/log/rocm-ernic/1.log
   2   worker    12346  running    2h14m      02:a1:b2:c3:d4:02  /run/rocm-ernic/2.sock         67891:2223   0B/0B           0B/0B           /var/log/rocm-ernic/2.log
   3   worker    12347  running    2h14m      02:a1:b2:c3:d4:03  /run/rocm-ernic/3.sock         -            -               -               /var/log/rocm-ernic/3.log
   4   worker    0      dead       -          02:a1:b2:c3:d4:04  /run/rocm-ernic/4.sock         -            -               -               /var/log/rocm-ernic/4.log

The VM column shows ``vm_pid:ssh_port`` when a VM is
attached, ``detached`` when detached, or ``-`` when no
VM is associated. The IP and RDMA columns show
TX/RX byte totals with auto-scaled units (B, K, M,
G, T). The UPTIME column shows how long each server
process has been running (``45s``, ``3m47s``,
``2h15m``, ``3d04h``).

Pass ``--json`` for machine-readable output (includes
raw ``ip_bytes_tx``, ``ip_bytes_rx``,
``rdma_bytes_tx``, ``rdma_bytes_rx`` fields).

Pass ``--rate SECS`` to show byte deltas measured over
a SECS-second window instead of cumulative totals.
This is useful with ``watch`` to monitor live
throughput.

ernicctl Command Reference
--------------------------

Service lifecycle
^^^^^^^^^^^^^^^^^

.. code-block:: bash

   ernicctl start                    # systemctl start
   ernicctl stop                     # systemctl stop
   ernicctl restart                  # systemctl restart
   ernicctl reload                   # re-read env
   ernicctl status [--json]          # instance table
   ernicctl status --rate 5          # 5s byte deltas
   ernicctl logs [N]                 # tail logs
   ernicctl stats [N]                # raw stats dump
   ernicctl stats --summary          # one-row summary

The ``ernicctl stats --summary`` command prints a
concise table with one row per instance:

.. code-block:: text

   ID  CONN        IP (TX/RX)      RDMA-SR (TX/RX) RDMA-RW (R/W)   MMIO (R/W)      CMDS    INTS    QPS  FLR
   1   connected   1.2K/3.4K       45M/12M         8.1M/22M        123K/456K       89      789     3    0
   2   connected   0B/0B           0B/0B           0B/0B           0B/0B           0       0       0    0

Columns: CONN = connection state, RDMA-SR = Send/Recv
bytes, RDMA-RW = Read/Write bytes, MMIO = register
reads/writes, CMDS = commands processed, INTS =
interrupts, QPS = active queue pairs, FLR = Function
Level Reset count.

Dynamic mesh management
^^^^^^^^^^^^^^^^^^^^^^^

Add or remove worker instances at runtime without
restarting the manager or existing workers:

.. code-block:: bash

   # Add a new worker to the TCP mesh
   sudo ernicctl add-instance

   # Remove instance 4 (detaches from VM if needed)
   sudo ernicctl remove-instance 4

Live VM attach and detach
^^^^^^^^^^^^^^^^^^^^^^^^^

Hot-plug or hot-unplug a server instance's vfio-user
device into/from a running VM via QMP:

.. code-block:: bash

   # Attach instance 2 to a running VM
   sudo ernicctl attach 2 \
     --qmp /run/rocm-ernic/qmp-2.sock

   # Detach instance 2 (optionally unload guest driver)
   sudo ernicctl detach 2 --unload-driver

The ``attach`` command accepts optional overrides:

- ``--device-id`` -- QEMU device ID
  (default ``ernicN``)
- ``--root-port`` -- PCIe root port bus
  (default ``pcie-vfu.1``)
- ``--ssh-port`` -- guest SSH port
- ``--ssh-user`` -- guest SSH user

Hot reload
^^^^^^^^^^

Perform a full hot-reload cycle for an instance: detach
from VM, stop server, rebuild, restart, re-attach, and
optionally update the guest driver:

.. code-block:: bash

   sudo ernicctl hot-reload 1
   sudo ernicctl hot-reload 1 --update-driver
   sudo ernicctl hot-reload 1 --no-build

This is the service-aware equivalent of the standalone
``scripts/hot-reload.sh`` script.

VM management
^^^^^^^^^^^^^

Launch, stop, and list QEMU VMs.  ``vm-launch`` uses
qemu-minimal's ``run-vm`` script with automatic QMP,
hotplug, and manifest integration:

.. code-block:: bash

   # Launch a VM connected to instance 1
   sudo ernicctl vm-launch 1

   # Launch with overrides
   sudo ernicctl vm-launch 2 \
     --vm-name my-test-vm \
     --ssh-port 2223 \
     --vcpus 8 --mem 16384

   # List running VMs
   ernicctl vm-list

   # Stop a VM
   sudo ernicctl vm-stop 1

The ``vm-launch`` command sets the following environment
variables for ``run-vm``:

- ``VFIO_USERDEV`` -- the instance's vfio-user socket
- ``QMP_SOCKET`` -- placed at
  ``$ERNIC_RUN_DIR/qmp-N.sock``
- ``QEMU_PATH``, ``VM_NAME``, ``IMAGES``, ``SSH_PORT``,
  ``VCPUS``, ``VMEM``

Driver distribution
^^^^^^^^^^^^^^^^^^^

Build a self-contained driver tarball and push it to
guest VMs:

.. code-block:: bash

   # Build the tarball
   sudo ernicctl driver-pack

   # Push to a VM and install automatically
   ernicctl driver-push ubuntu@localhost \
     --port 2222 --install

   # Push without auto-install
   ernicctl driver-push ubuntu@localhost --port 2222

The tarball extracts to ``/tmp/rocm-ernic-driver/`` on
the guest and includes ``vm-driver-install.sh`` which:

1. Builds ``rocm_ernic_eth.ko`` and
   ``rocm_ernic_rdma.ko`` against the running kernel.
2. Loads ``ib_core`` and ``ib_uverbs``.
3. Inserts both driver modules.
4. Verifies the RDMA device appears via
   ``ibv_devices``.

The install script also supports ``--unload`` to remove
the modules, ``--build-only`` to compile without
loading, and ``--dkms`` to use DKMS instead of
``insmod``.

Manifest
--------

The service maintains a JSON manifest at
``$ERNIC_RUN_DIR/instances.json`` that tracks all
running instances and their VM attachment state.  The
manifest is the single source of truth for ``ernicctl``
and the launcher.

Example:

.. code-block:: json

   {
     "instances": [
       {
         "id": 1,
         "role": "manager",
         "pid": 12345,
         "mac": "02:a1:b2:c3:d4:01",
         "socket": "/run/rocm-ernic/1.sock",
         "vm": {
           "qmp_socket": "/run/rocm-ernic/qmp-1.sock",
           "device_id": "ernic1",
           "root_port": "pcie-vfu.1",
           "ssh_port": 2222,
           "ssh_user": "ubuntu",
           "attached": true,
           "vm_pid": 67890,
           "vm_name": "stebates-test-vm"
         }
       }
     ]
   }

When ``vm`` is ``null``, the instance is running but not
attached to any VM.

Typical Workflows
-----------------

Single-server development
^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: bash

   # Edit /etc/rocm-ernic/rocm-ernic.env:
   #   ERNIC_INSTANCES=1
   sudo systemctl start rocm-ernic
   sudo ernicctl vm-launch 1
   ernicctl driver-push ubuntu@localhost \
     --port 2222 --install
   ssh -p 2222 ubuntu@localhost

Multi-server TCP mesh
^^^^^^^^^^^^^^^^^^^^^

.. code-block:: bash

   # Edit /etc/rocm-ernic/rocm-ernic.env:
   #   ERNIC_INSTANCES=3
   sudo systemctl start rocm-ernic
   ernicctl status

   # Launch VMs for instances 1 and 2
   sudo ernicctl vm-launch 1
   sudo ernicctl vm-launch 2

   # Push driver to both VMs
   ernicctl driver-push ubuntu@localhost \
     --port 2222 --install
   ernicctl driver-push ubuntu@localhost \
     --port 2223 --install

Adding a server to a running mesh
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: bash

   sudo ernicctl add-instance
   # Prints: Added instance 4.
   sudo ernicctl vm-launch 4
   ernicctl status

Hot-reloading after a code change
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: bash

   # Rebuild server and cycle instance 1
   sudo ernicctl hot-reload 1 --update-driver

Guest VM Hardening
^^^^^^^^^^^^^^^^^^

The ``vm-driver-install.sh`` script automatically
configures systemd to avoid long shutdown hangs
caused by stale user session scopes:

- ``KillUserProcesses=yes`` in
  ``/etc/systemd/logind.conf``
- ``DefaultTimeoutStopSec=15s`` in
  ``/etc/systemd/system.conf``

These settings are applied on first driver install
and persist across reboots.
