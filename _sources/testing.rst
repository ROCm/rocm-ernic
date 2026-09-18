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

``site.yml`` runs five plays in order:

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
