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
- PCI Device ID verification (ROCm ERNIC: ``0x100a``)
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
- PCI identity is ``0x1dd8:0x100a``
- BAR geometry is 64 KB BAR0 (32 KB register window) and
  4 MB BAR2, with 32 MSI-X vectors
- Clean shutdown on ``SIGTERM``
- The stats file carries the full counter set
- ``--tap`` attaches to an existing host TAP

The last check is skipped unless ``ERNIC_TEST_TAP`` names a
TAP interface owned by the current user, since creating one
needs ``CAP_NET_ADMIN``:

.. code-block:: bash

   sudo ip tuntap add dev ernic-ci0 mode tap user "$USER"
   ERNIC_TEST_TAP=ernic-ci0 ctest --test-dir build -R ionic-ci

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

Ansible-Based Testing
---------------------

The ``ansible/`` directory contains playbooks that automate
the multi-VM test workflow: building the server, installing
the systemd service, provisioning already-running guests
with the driver and the ionic rdma-core provider, and
running iperf3, perftest and NVMe-oF tests against them. The
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

A single command builds, deploys, and tests everything:

.. code-block:: bash

   cd ansible
   ansible-playbook site.yml

The guests themselves are not created here. Bring them up
first with ``ernicctl vm-launch`` (or ``ci/jobs/vm-up.sh``),
having fetched the published guest image once with
``scripts/fetch-guest-image.sh``; see
:ref:`ansible-guest-image` below.

This then runs five plays in order:

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
5. **performance-tests** -- the TCP and RDMA sweeps.

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
   ansible-playbook playbooks/nvmeof-tests.yml
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

The kernel floor is two-sided.
``drivers/infiniband/hw/ionic`` merged in 6.18, which
``ernic_ionic_min_kernel`` asserts against the running
guest; and ``ionic-ernic`` calls ``ib_umem_get_va``, which
landed after 7.0, so a 7.0 guest passes the floor check and
then fails the DKMS build. ``ernic_guest_setup`` compares
the guest kernel's major.minor against ``IONIC_KERNEL_REF``
before it starts that build.

The ``CI guest kernel`` badge on line 9 of ``README.md`` has
to be hardcoded -- shields.io cannot read the image -- so
without a check a tag bump would leave the front page
advertising a kernel nothing ships. That comparison runs in
the ``Read VM info`` step of the loopback job in
``.github/workflows/system-tests.yml``, on every pull
request.

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
``patches/*.patch`` with ``git am``, failing the pull
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
drives the same guest-setup, sanity and performance plays
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
