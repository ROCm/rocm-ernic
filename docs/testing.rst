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
- PCI Vendor ID verification (Pensando: ``0x1dd8``)
- PCI Device ID verification (ROCm ERNIC: ``0x1002``)
- PCI subsystem verification (ROCm ERNIC: ``0x1dd8:0x5400``)
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

Requires an RDMA device (via the guest ``ionic_rdma`` driver
or real hardware). Skipped if no device is found.

find-rdma-device-tuple
^^^^^^^^^^^^^^^^^^^^^^^

A shell test registered with CTest. It uses a synthetic sysfs tree to verify
RDMA discovery requires the emulated vendor/device/subsystem tuple and rejects
near matches.

find-ipv4-gid-index
^^^^^^^^^^^^^^^^^^^

A shell test registered with CTest. It uses synthetic sysfs GID tables to
verify ``scripts/find-ipv4-gid-index.sh`` finds an address's IPv4-mapped
GID at any index and rejects malformed arguments.

test_rdma_cm
^^^^^^^^^^^^

RDMA Connection Manager test using libibverbs. Validates
connection setup and teardown paths.

test_ionic_ci.sh
^^^^^^^^^^^^^^^^

Shell test for the ionic emulation path, registered with
CTest as ``ionic-ci``. It needs no VM and no RDMA device:

- Server starts on the ``loopback`` and ``none`` backends,
  and with no extra flags at all
- PCI identity is ``0x1dd8:0x1002``
- BAR geometry is 64 KB BAR0 (32 KB register window) and
  4 MB BAR2, with 32 MSI-X vectors under ``compat`` and 97
  under ``generic-400g``
- Clean shutdown on ``SIGTERM``
- The stats file carries the full counter set
- ``--tap`` attaches to an existing host TAP

The last check is skipped unless ``ERNIC_TEST_TAP`` names a
TAP interface owned by the current user, since creating one
needs ``CAP_NET_ADMIN``:

.. code-block:: bash

   sudo ip tuntap add dev ernic-ci0 mode tap user "$USER"
   ERNIC_TEST_TAP=ernic-ci0 ctest --test-dir build -R ionic-ci

In-process ionic device harness
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``ernic-test-ionic-harness`` runs the whole emulated device
inside the test process. It links every source the server is
built from except ``rocm_ernic_server.c``, and replaces
libvfio-user with ``tests/fake_vfu.c``, which keeps guest
memory in heap buffers and counts interrupts instead of
delivering them. ``tests/ionic_harness.c`` then plays the
guest driver: devcmds through BAR0, doorbells through BAR2,
and rings, work requests and completions in guest memory.
It needs no VM, socket or RDMA device, so it runs in every
CI build leg, sanitizers included.

Each group of checks is its own CTest test:

- ``ionic-harness-devcmds``, ``-bringup``: the devcmds, the
  Ethernet admin queue, the RDMA bootstrap, and MSI-X
  masking
- ``ionic-harness-admin``: admin commands the device must
  refuse
- ``ionic-harness-rc``, ``-atomic``, ``-cq-events``: sends,
  RDMA reads and writes, atomics and fast registration
  between two RC QPs on the loopback backend, and CQ arming
- ``ionic-harness-ud``, ``-srq``: datagrams, address handles
  and shared receive queues
- ``ionic-harness-eth``: Ethernet transmit and receive
- ``ionic-harness-nvmeof``: the IB CM handshake on GSI, then
  a Fabrics Connect and an Identify on the RC QP it binds

.. code-block:: bash

   ctest --test-dir build -R ionic-harness

To run one group directly, pass its name, for example
``build/tests/ernic-test-ionic-harness rc``. Setting
``ERNIC_FAKE_VFU_LOG_LEVEL`` to a syslog level (``7`` for
everything) prints the device's ``vfu_log()`` messages.

Driving a server from Python
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``tests/py/`` stands in for a guest VM. Where the harness
above replaces libvfio-user, these suites keep it: they speak
the vfio-user protocol to a real ``rocm-ernic`` process over
its Unix socket, as a VMM does. Guest memory is a ``memfd``
that the client mmaps and hands to the server in a
``DMA_MAP``, so the server maps the same pages and the real
DMA path runs. Nothing here needs a VM, RDMA hardware, root,
or any package outside the standard library.

The modules under ``tests/py/ernic/`` are:

- ``vfu.py`` -- the vfio-user client and ``GuestMemory``
- ``server.py`` -- starting server processes, holding them to
  a shutdown deadline, and counting the descriptors they hold
- ``ionic.py`` -- the ionic driver, the Python counterpart of
  ``tests/ionic_harness.c``
- ``mesh.py`` -- the TCP mesh wire protocol, a fake manager,
  and a fixture that starts a manager and some workers
- ``check.py`` -- the small test runner the suites report
  through

Four suites are registered with CTest:

- ``ernic-vfu-client``: version negotiation, device and
  region info, PCI configuration space, BAR access, DMA
  mapping, interrupts, reset and reconnect -- and the
  offsets, counts, addresses and vector ranges a client must
  not get away with
- ``ernic-ionic-driver``: bring-up, connected queue pairs,
  memory regions whose pages are deliberately out of order,
  sends with several SGEs or an inline payload, RDMA reads
  and writes, shared receive queues, and the keys and lengths
  that must fail
- ``ernic-ionic-hw-stats``: the hardware statistics behind a
  guest's ``hw_counters`` -- the ``STATS_HDRS``,
  ``STATS_VALS``, ``QP_STATS_HDRS`` and ``QP_STATS_VALS``
  tables, that they describe themselves the way the driver
  reads them, that a buffer too small for the table gets
  whole entries only, that values for a QP that does not
  exist are refused rather than zero-filled, and that the
  ``--stats-file`` the exporter reads carries the same
  numbers. It drives a SEND, an RDMA WRITE and an RDMA READ
  of three distinct sizes, and checks that every counter in
  both tables moves by exactly the right amount on the right
  QP. The structural checks read whatever set the device
  advertises, so a counter added to either table is covered
  without touching the test
- ``ernic-mesh-stress``: a three-node mesh under traffic
  between every pair of nodes, peers that send frames no real
  node sends, the protocol version gate in both directions,
  and nodes killed and restarted while the manager's
  descriptors are counted; then, on a second mesh with a 1 s
  health-check interval, a worker and then the manager
  paused past the health check, after which the workers
  have to register again under their own ids

.. code-block:: bash

   ctest --test-dir build -R '^ernic-(vfu|ionic|mesh)'

To run one directly, and to vary the mesh:

.. code-block:: bash

   python3 -I tests/py/test_mesh_stress.py \
       --server build/rocm-ernic --size 5

Like the shell suites, they run the server under the command
in ``ERNIC_TEST_SERVER_WRAPPER``, so CI's valgrind leg
covers them too.

Two devices in one process
^^^^^^^^^^^^^^^^^^^^^^^^^^

``ernic-test-gsi-mesh`` (CTest ``gsi-mesh``) builds the same
harness twice: one device as the mesh manager and one as a
worker, joined over loopback TCP by the real TCP backend. It
checks that the two register, that a GID claimed *after* the
mesh has formed is advertised and resolves to the claiming
node from the other side, and that neither node resolves its
peer's address to itself. It then sends a CM-shaped MAD on
QP1 and checks both that it reaches the peer's QP1 rather
than coming back round, and that it arrives behind the right
header: the two nodes' GIDs are v4-mapped, so a forwarded
datagram has to look like RoCEv2 over IPv4, flagged as such,
with the *sender's* address as its source. Forwarding the MAD
correctly and describing it wrongly looks identical from
every other angle, and is what used to strand ``rdma_cm``.

That is the path ``rdma_cm`` depends on and the one
``ERNIC_TCP_GUEST_GIDS`` cannot cover: it is read once at
startup, and one environment variable cannot describe two
devices in one process. ``test_ud_mesh`` covers the same
journey for a UD datagram, but needs two VMs and two real
devices, so it cannot run in CI.

.. code-block:: bash

   ctest --test-dir build -R gsi-mesh

``ERNIC_TEST_MESH_PORT`` moves the port it listens on, for a
runner that already has something on the default.

NVMe-oF tests
^^^^^^^^^^^^^

Three tests cover the in-process NVMe-oF controller without
needing a VM: ``nvmeof-target-unit`` (capsule handling and
the command set), ``nvmeof-cm-unit`` (the IB CM state
machine) and ``nvmeof-ci`` (a shell test that starts the
real server once per documented option spelling and checks
what it reports, including that bad options are refused).

.. code-block:: bash

   ctest --test-dir build -R nvmeof

The guest-side connect needs a VM. It lives in
``ansible/playbooks/nvmeof-tests.yml``, which the hosted
``system-test-nvmeof`` job runs end to end and which
``ci/jobs/vm-nvmeof.sh`` mirrors check-by-check for the
self-hosted lane. See :doc:`nvmeof`.

S3-over-RDMA tests
^^^^^^^^^^^^^^^^^^

Five tests cover the in-process object store without needing
a VM: ``s3-token-unit`` (the RDMA token wire format),
``s3-http-unit`` (the HTTP/1.1 subset), ``s3-target-unit``
(the store and its transfers against fake DMA ops),
``s3-tcp-unit`` (the in-band ARP/ICMP/TCP stack over a
loopback) and ``s3-ci`` (a shell test that starts the real
server once per documented option spelling).

.. code-block:: bash

   ctest --test-dir build -R '^s3-'

The guest-side transfer needs a VM, because it needs a real
``ibv_reg_mr`` for the token to describe.
``tests/s3_rdma_client.c`` is built and run inside the guest
by ``ansible/playbooks/s3-tests.yml``, which the hosted
``system-test-s3`` job runs end to end and which
``ci/jobs/vm-s3.sh`` mirrors check-by-check for the
self-hosted lane. See :doc:`s3`.

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

To run the tests under valgrind's memcheck, as the Build and
Test workflow's GCC Debug leg does, use a GCC build without the
sanitizers and with valgrind installed before configuring. Leave
out the tests that start the server or run a script, which
valgrind would only check the script for:

.. code-block:: bash

   skip='pci-config-test|find-rdma-device-tuple|find-ipv4-gid-index'
   skip+='|server-lifecycle|ionic-server-smoke|ionic-ci|tcp-mesh-ci'
   skip+='|nvmeof-ci|s3-ci|ionic-stats-interval-cli|ionic-eth-queues-cli'
   skip+='|cmake-subproject|ernic-package-deb'
   opts='--leak-check=full --track-origins=yes --num-callers=40'
   opts+=' --error-exitcode=1'
   ctest --test-dir build -T memcheck --output-on-failure \
     --overwrite "MemoryCheckCommand=$(command -v valgrind)" \
     --overwrite "MemoryCheckCommandOptions=$opts" \
     -E "^($skip)$"

Each test's valgrind report is in
``build/Testing/Temporary/MemoryChecker.<n>.log``.

The test scripts that start the server run it under the command
in ``ERNIC_TEST_SERVER_WRAPPER``, if that's set, as does
``tests/test_loopback_ci.sh``. The command is split on
whitespace, so no word of it can contain a space.
``test_server_lifecycle`` doesn't read it; run that program under
valgrind with ``--trace-children=yes``, which follows it into
each server it starts. To run the server under valgrind, as the
same leg does, have valgrind write a log for each process, since
the scripts don't all check how the server exited. With ``opts``
as above:

.. code-block:: bash

   srv='pci-config-test|ionic-server-smoke'
   srv+='|ionic-ci|tcp-mesh-ci|nvmeof-ci|s3-ci'
   srv+='|ionic-stats-interval-cli|ionic-eth-queues-cli'
   logs="$PWD/build/valgrind-server"
   rm -rf "$logs"
   mkdir "$logs"
   opts+=" --log-file=$logs/%p.log"
   export ERNIC_TEST_SERVER_WRAPPER="valgrind $opts"
   ctest --test-dir build --output-on-failure -R "^($srv)$"
   valgrind --trace-children=yes $opts build/tests/test_server_lifecycle
   grep -L 'ERROR SUMMARY: 0 errors' "$logs"/*.log

The last command lists the logs with a report.

Multi-VM RDMA Testing
---------------------

With two VMs launched via ``ernicctl``, you can run
standard RDMA benchmarks over the emulated NICs.

Prerequisites:

1. One host TAP per instance,
   all enslaved to a shared bridge, so the guests can
   reach each other over IP (see :doc:`ionic`):

   .. code-block:: bash

      sudo ip link add ernicbr0 type bridge
      sudo ip link set ernicbr0 up
      for n in 1 2; do
        sudo ip tuntap add dev "ernic-tap${n}" mode tap \
          user "$USER"
        sudo ip link set "ernic-tap${n}" master ernicbr0 up
      done

   The ``ernic_host_setup`` Ansible role does this for you.
2. Start the rocm-ernic service and launch two VMs
   (see :doc:`service`).
3. Install the guest drivers and rdma-core v62 in both VMs
   (see ``ernicctl driver-push``).
4. Configure IP addresses on the rocm-ernic NICs
   (``enp1s0``) in both VMs.
5. In each VM, find the index of the IPv4-mapped GID
   (``::ffff:a.b.c.d``) for its address, which the examples
   below pass as ``$GID``. It is often 1, but it isn't
   fixed, and the two VMs can differ.
   ``scripts/find-ipv4-gid-index.sh`` looks it up; run it in
   each guest with the device and that guest's own address,
   for example from the host:

   .. code-block:: bash

      GID=$(ssh vm1 'sh -s -- rocep1s0 192.168.100.10' \
        < scripts/find-ipv4-gid-index.sh)

ibv_rc_pingpong
^^^^^^^^^^^^^^^

Latency test using RC (Reliable Connection) QPs:

.. code-block:: bash

   # VM 1 (server):
   LD_LIBRARY_PATH=/opt/rdma-core-ernic/lib \
     ibv_rc_pingpong -d rocep1s0 -g "$GID" -n 100

   # VM 2 (client, use multicast NIC for OOB):
   LD_LIBRARY_PATH=/opt/rdma-core-ernic/lib \
     ibv_rc_pingpong -d rocep1s0 -g "$GID" -n 100 \
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
     ib_send_bw -d rocep1s0 -x "$GID" -n 10 \
     --report_gbits

   # VM 2 (client):
   LD_LIBRARY_PATH=/opt/rdma-core-ernic/lib \
     ib_send_bw -d rocep1s0 -x "$GID" -n 10 \
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

Ansible-Based Testing
---------------------

The ``ansible/`` directory contains playbooks that automate
the multi-VM test workflow: building the server, installing
the systemd service, provisioning already-running guests
with the driver and the ionic rdma-core provider, and
running iperf3, perftest, RDMA-Tutorial and NVMe-oF tests
against them. The
guest disk is not built here; see :ref:`ansible-guest-image`.

Prerequisites
^^^^^^^^^^^^^

- ``ansible-core`` 2.18+, which is what ``community.general``
  13 needs. Ubuntu 24.04's ``ansible`` package ships 2.16;
  use ``pipx install ansible-core`` or the Ansible PPA.
- The ``community.general`` Galaxy collection, which is all
  ``requirements.yml`` resolves to

.. code-block:: bash

   cd ansible
   ansible-galaxy collection install \
     -r requirements.yml

Running the full workflow
^^^^^^^^^^^^^^^^^^^^^^^^^

**The guests must be up and running before you start.**
``site.yml`` provisions and tests VMs; it never creates them,
and every play after the first one fails without them.

It is not quite a single command, because of the order the
phases impose. Phase 1 installs the service that launches the
guests, so it has to run before there are any; and it also
*stops* whatever is running, unless ``ernic_restart_existing``
is false. Launching the VMs and then running ``site.yml``
plain therefore tears down the guests the run needs, and
``vm-register`` fails with no VM attached to any instance.

Fetch the published guest image once with
``scripts/fetch-guest-image.sh`` (see
:ref:`ansible-guest-image` below), then:

.. code-block:: bash

   cd ansible

   # 1. Build, install the service and ernicctl.
   ansible-playbook site.yml --tags host-setup

   # 2. Attach a VM to each instance.
   sudo ernicctl vm-launch 1

   # 3. Provision the guests and run the tests, leaving
   #    the VMs from step 2 alone.
   ansible-playbook site.yml -e ernic_restart_existing=false

On later runs, steps 1 and 3 collapse back into the single
``ansible-playbook site.yml -e ernic_restart_existing=false``
for as long as the guests stay up. Dropping the override is
what you want when you *do* mean to recycle the mesh --- it
stops the VMs and restarts the service, after which step 2
has to be repeated.

VMs brought up by ``ci/jobs/vm-up.sh`` are the CI path and
run unprivileged. Drive those with ``ci-site.yml``, which
imports no host-setup play and so neither tears them down nor
touches the systemd service.

``site.yml`` runs six plays in order:

1. **host-setup** -- builds the project, installs the
   service and ``ernicctl``, templates the env file, and
   starts the service.
2. **vm-register** -- reads the launcher's
   ``instances.json`` and adds each running VM to the
   ``ernic_vms`` group with its SSH port, user and NIC
   address. Everything below depends on it.
3. **guest-setup** -- installs the ionic rdma-core
   provider when the image does not already carry a
   matching one, builds and loads the guest driver from
   the ionic DKMS package, and assigns IPs to the
   emulated NICs.
4. **sanity-tests** -- runs ``iperf3`` between two VMs
   for TCP/IP validation and ``ib_send_bw`` /
   ``ibv_rc_pingpong`` for RDMA verification.
5. **tutorial-tests** -- builds examples from the
   RDMA-Tutorial and runs each between the two VMs; see
   :ref:`ansible-rdma-tutorial`.
6. **performance-tests** -- the TCP and RDMA sweeps.

The setup plays are thin wrappers around the roles of the
``sbates130272.rocm_ernic`` collection, whose source lives in
``ansible/roles/``; see :ref:`ansible-collection` below.

Running individual plays
^^^^^^^^^^^^^^^^^^^^^^^^

Each play can also be run separately:

.. code-block:: bash

   ansible-playbook playbooks/host-setup.yml
   ansible-playbook playbooks/vm-register.yml
   ansible-playbook playbooks/guest-setup.yml
   ansible-playbook playbooks/sanity-tests.yml
   ansible-playbook playbooks/tutorial-tests.yml
   ansible-playbook playbooks/nvmeof-tests.yml
   ansible-playbook playbooks/s3-tests.yml
   ansible-playbook playbooks/performance-tests.yml

Variable overrides
^^^^^^^^^^^^^^^^^^

Override any default from ``group_vars/all.yml`` with
``-e``:

.. code-block:: bash

   # Four instances instead of two
   ansible-playbook site.yml -e ernic_instances=4

   # Skip the build (use existing install)
   ansible-playbook site.yml -e ernic_build=false

   # Skip sanity tests
   ansible-playbook site.yml -e ernic_tests=false

   # Pin a different published image
   ansible-playbook site.yml \
     -e ernic_vm_artifact_tag=<tag>

   # Use a backing image you staged yourself
   ansible-playbook site.yml \
     -e ernic_vm_backing=/path/to/backing.qcow2

``group_vars/all.yml`` holds the site configuration for this
repo; the per-role defaults live in each role's
``defaults/main.yml`` under the collection described below.
Anything set in ``group_vars/all.yml`` wins over a role
default. See ``ansible/PLAYBOOKS.md`` for additional usage
notes.

.. _ansible-rdma-tutorial:

RDMA-Tutorial examples
^^^^^^^^^^^^^^^^^^^^^^

``playbooks/tutorial-tests.yml`` runs examples from
`RDMA-Tutorial <https://github.com/jcxue/RDMA-Tutorial>`_,
a short introduction to programming with verbs, to check
that code written the way it teaches works over rocm-ernic.
It covers example 1 (RC send/recv) and example 2 (RC RDMA
write), with the server in VM 1 and the client in VM 2.

The tutorial keeps each example as a commit, and they
address the peer by InfiniBand LID. The play pins each
example to its commit and applies a patch from
``ansible/playbooks/files/rdma-tutorial/`` that exchanges
the peers' IPv4-mapped GIDs instead, as RoCE needs, and uses
the port's active MTU. It also sets ``SO_REUSEADDR`` on the
server's socket, so a rerun can bind the same port while the
last run's connection is still in ``TIME_WAIT``. The
controller clones the tutorial and applies the patches, and
copies the result into the guests, which build it, so they
need neither network access nor git.
As with perftest, the IPv4-mapped GID reaches a peer
only if its instance advertises the guest's IPv4 address
(``ERNIC_GUEST_GIDS_<n>``, or ``ernic_guest_gids`` in the
host-setup role), as both CI lanes do.

An example passes when the client and the server both exit
0 and each has logged its throughput, which it does only
after its last operation. Upstream runs 5 or 10 million
operations, which take tens of minutes over the emulated NIC;
``ernic_rdma_tutorial_ops`` (default 10000) sets the count.
``ernic_rdma_tutorial_timeout`` bounds each process and
``ernic_rdma_tutorial_port`` picks the ports they connect on.

Both CI lanes that run the sanity play run this one after
it: the two-VM job in ``system-tests.yml`` and
``ci/jobs/vm-functional.sh``.

.. _ansible-guest-image:

The guest image
^^^^^^^^^^^^^^^

Nothing in ``ansible/`` builds a guest image. The guests
come from a published OCI artifact: the ``ionic`` flavour of
`batesste-ci-images
<https://github.com/sbates130272/batesste-ci-images>`_,
which pins mainline kernel 7.2.3 on Ubuntu 26.04 (resolute)
and bakes the RDMA userspace, the DKMS toolchain,
``perftest`` and a distro rdma-core 61.0 carrying the ionic
provider.

``scripts/fetch-guest-image.sh`` pulls the image and its
``vm-info.json`` metadata with ``oras``, decompresses the
qcow2 and runs ``qemu-img check`` over it. The download is
skipped when the tag is already unpacked. ``ci/jobs/vm-up.sh``
calls the same script, so the lab host and CI land the same
bytes in the same layout.

It also checks the image's ``vm-info.json`` against this
checkout and refuses the image when they disagree. That
happens between the metadata pull and the disk pull, so a
rejection costs kilobytes rather than the 3.7 GB it would
cost after -- and every assertion reads either the metadata
or a file in this checkout, so pulling the disk first could
not change the verdict anyway. Verification reruns on the
cached path too, because repinning ``IONIC_KERNEL_REF``
invalidates a directory that was correct when it was pulled.
``--no-verify`` skips the lot.

The kernel floor it enforces is two-sided.
``drivers/infiniband/hw/ionic`` merged in 6.18, which
``ernic_ionic_min_kernel`` also asserts against the running
guest; and ``ionic-ernic`` calls ``ib_umem_get_va``, which
landed after 7.0, so a 7.0 guest clears the floor and then
fails the DKMS build. The script compares the image kernel's
major.minor against ``IONIC_KERNEL_REF`` in
``cmake/ErnicKernelModule.cmake`` -- as does
``ernic_guest_setup``, against the kernel the guest actually
booted, before it starts that build.

The ``CI guest kernel`` badge on line 9 of ``README.md`` has
to be hardcoded -- shields.io cannot read the image -- so
without a check a tag bump would leave the front page
advertising a kernel nothing ships. Two independent
implementations compare them: the fetch script, on the
self-hosted and manual paths, and an inline copy in the
``Read VM info`` step of the loopback job in
``.github/workflows/system-tests.yml``, on every pull
request. The hosted jobs pull through
``.github/actions/fetch-guest-vm``, not this script, so
neither copy is redundant.

Four more properties are site expectations rather than
repo-derivable facts, so the caller supplies them:
``--expect-user``, ``--expect-disk``, ``--expect-release``
and ``--expect-flavour``. ``ci/lib/common.sh`` passes all
four, which is what catches a ``gpu``-flavour image reaching
a lane whose roles want the ionic userspace. These
assertions used to live in ``playbooks/vm-fetch.yml``, which
0.2.0 removed; in the script they cover every caller, not
just the one that ran a play.

Keep ``ernic_vm_artifact_tag`` equal to
``GUEST_ARTIFACT_TAG`` in
``.github/workflows/system-tests.yml`` *and* to
``CI_GUEST_ARTIFACT_TAG`` in ``ci/lib/common.sh``. Nothing
compares those three to each other.

The pinned image is flavour ``ionic`` and carries no ROCm,
so ``ernic_gpu_passthrough`` defaults off alongside it. A
GPU rig needs a GPU-flavoured image.

.. _ansible-collection:

The rocm_ernic Ansible collection
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The reusable parts of the automation are packaged as the
``sbates130272.rocm_ernic`` Galaxy collection, so any
suitable guest can be turned into a rocm-ernic RDMA node
without the playbooks here:

``ernic_guest_setup``
   Installs the DKMS driver, builds rdma-core with the
   ``rocm_ernic`` provider, applies the udev rules, addresses
   the emulated NIC and builds rocm-xio.

``ernic_host_setup``
   Builds, installs and starts the rocm-ernic service, binds
   GPUs to ``vfio-pci`` and stages rocm-xio for the guests.

``ernic_source``
   Resolves the rocm-ernic checkout the others copy from,
   cloning it on the controller when ``ernic_source_dir`` is
   not set.

Consumers outside this repo install it from Galaxy and address
the roles by their fully qualified name:

.. code-block:: bash

   ansible-galaxy collection install sbates130272.rocm_ernic

.. code-block:: yaml

   roles:
     - role: sbates130272.rocm_ernic.ernic_guest_setup

Playbooks in this repo instead reach the same roles by short
name through ``roles_path`` in ``ansible/ansible.cfg``, so they
always run against this checkout rather than a published
version. Because that path is relative, run ``ansible-playbook``
from the ``ansible/`` directory.

Guest Driver CI
---------------

``.github/workflows/driver-build.yml`` covers the guest
driver. Its ``ionic-patches`` job reads
``IONIC_KERNEL_REF`` straight out
of ``cmake/ErnicKernelModule.cmake``, sparse-clones the two
ionic subtrees at that ref, and applies every
``ansible/files/patches/*.patch`` with ``git am``,
failing the pull
request if one no longer applies. The ionic modules
themselves are not built there: they need headers matching
``IONIC_KERNEL_REF`` --- the sources track IB-core helpers
that move between minor releases, so the guest kernel's
major.minor must equal the ref's, and no hosted runner
carries such a kernel.

``.github/workflows/system-tests.yml`` boots guests under
KVM on hosted runners and provisions them by running the
collection itself --- ``ansible-playbook ci-site.yml --tags
guest-setup`` against a generated ``instances.json`` ---
rather than by copying sources in over ``ssh``. The role is
therefore exercised on every pull request, and the guest
build in CI is the same one a ``site.yml`` run
produces. The workflow installs only kernel headers and the
build toolchain before handing over; the mainline kernel
itself must already be in the guest image, and
``ernic_guest_setup`` asserts that it matches the pinned ref
before it starts the DKMS build.

Self-Hosted CI
--------------

The GitHub-hosted workflows can only build and unit-test.
Anything needing KVM, a provisioned guest image, or two
guests exchanging RDMA traffic runs on a self-hosted runner
instead, driven by the harness in ``ci/``.

It runs in three tiers:

============  ===============================  =========
Tier          Scope                            Needs KVM
============  ===============================  =========
1             build, ctest, loopback backend   no
2             two-VM RDMA functional           yes
3             performance sweeps               yes
============  ===============================  =========

Tiers 2 and 3 are scheduled onto runners carrying the
``kvm`` label, so a node without KVM access stops
attracting those jobs rather than failing them.

The harness runs entirely unprivileged. The launcher and
``ernicctl`` are environment-driven, so the control plane
is redirected under a workspace the CI user owns rather
than ``/run``, ``/var/log`` and ``/usr/local``.

Test logic is not duplicated: ``ansible/ci-site.yml``
drives the same guest-setup, sanity, RDMA-Tutorial and
performance plays
described above, supplying only the inventory
registration that ``site.yml`` would normally provide.

Check whether a node is ready:

.. code-block:: bash

   ci/doctor.sh

Run any tier by hand:

.. code-block:: bash

   bash ci/jobs/build.sh
   bash ci/jobs/loopback.sh
   bash ci/jobs/vm-up.sh
   bash ci/jobs/vm-functional.sh
   bash ci/jobs/perf.sh
   bash ci/jobs/vm-down.sh

Results are merged into a functional and performance
report by ``ci/report/gen-report.py``, which also checks
medians against a stored baseline and exits non-zero on
regressions. See ``ci/README.md`` for node setup,
registration and the security notes that apply because
this is a public repository.

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
