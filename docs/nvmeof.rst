NVMe-oF Controller
==================

The ``nvmeof`` backend turns a rocm-ernic instance into an
NVMe over Fabrics *target*. The controller lives inside the
server process and is reached over the emulated RDMA fabric,
so a stock guest can run

.. code-block:: bash

   sudo nvme connect -t rdma -a 192.168.200.1 -s 4420 -n nvmet-test

and get a working ``/dev/nvmeXnY`` with no second virtual
machine, no second server instance, and no RDMA hardware
anywhere in the picture.

That is the point of it. Every other RDMA path in this
project needs two endpoints to prove anything: two guests,
or a guest and a peer server over the TCP mesh. Here one
guest and one server are a complete fabric, which makes
NVMe-oF usable as a day-to-day smoke test of the queue
pairs, the memory keys, and the RDMA READ/WRITE engine in a
single VM.

Measured throughput and latency are on a page of their own:
see :doc:`nvmeof-performance`, which also documents the
memory-region budget that caps ``queues=`` at 15
(:ref:`nvmeof-mr-budget`).

Starting the Server
-------------------

.. code-block:: bash

   # 64 MiB RAM namespace, 512-byte blocks, on 192.168.200.1:4420
   ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend nvmeof --tap ernic0

   # 1 GiB with 4 KiB blocks
   ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend nvmeof:size=1G,bs=4096 --tap ernic0

   # File-backed namespace that survives a restart
   ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend nvmeof:file=/var/tmp/ns0.img,size=1G --tap ernic0

The backend string is ``nvmeof`` optionally followed by
``:`` and a comma-separated option list:

.. list-table::
   :header-rows: 1
   :widths: 12 18 70

   * - Option
     - Default
     - Meaning
   * - ``size=``
     - ``64M``
     - Namespace capacity. Accepts ``K``/``M``/``G``/``T``
       and the ``KiB``/``MiB`` spellings; must be a whole
       number of blocks. Without it, an existing ``file=``
       image keeps its own size.
   * - ``bs=``
     - ``512``
     - Logical block size. One of 512, 1024, 2048 or 4096.
   * - ``file=``
     - unset
     - Back the namespace with this regular file instead
       of anonymous memory. A new or empty file is created
       at ``size=``. An existing image is never shrunk: a
       ``size=`` larger than the image grows it, a smaller
       one is refused, and without ``size=`` the image has
       to be a whole number of blocks. See
       :ref:`nvmeof-durability`.
   * - ``nqn=``
     - ``nvmet-test``
     - Subsystem NQN the guest connects to.
   * - ``ip=``
     - ``192.168.200.1``
     - IPv4 address the controller answers on. It has to
       be routable from the guest's ``rocm-ernic0``
       interface.
   * - ``port=``
     - ``4420``
     - Transport service ID.
   * - ``queues=``
     - ``8``
     - Maximum I/O queues, 1..15. The ceiling is the
       emulator's memory-region table, not the NVMe
       spec: an ``nvme-rdma`` initiator pre-allocates
       128 regions per queue at connect time, admin
       queue included, out of a table of 2048.
   * - ``nsid=``
     - ``1``
     - Namespace identifier.
   * - ``model=``
     - ``rocm-ernic NVMe-oF target``
     - Identify-controller model string, 1 to 40
       printable ASCII characters.
   * - ``serial=``
     - ``ERNIC0000000000001``
     - Identify-controller serial number, 1 to 20
       printable ASCII characters.

A malformed option is refused before the device is
presented, rather than surfacing later as a failed connect:

.. code-block:: console

   $ ./build/rocm-ernic --backend nvmeof:size=bogus
   Error: nvmeof backend: bad size 'bogus'

A successful start logs the controller at ``info``:

.. code-block:: console

   ionic_datapath: nvmeof target 'nvmet-test' 1073741824 bytes bs=4096 \
       at 192.168.200.1:4420

The size it logs is the one settled on, which for an existing
``file=`` image without ``size=`` is the image's.

.. _nvmeof-durability:

File-backed namespaces
^^^^^^^^^^^^^^^^^^^^^^

A ``file=`` image may hold an earlier run's data, so the
server never shrinks it. One it can't use without losing data
is refused when the device is created, after the options have
been accepted:

.. code-block:: console

   $ ./build/rocm-ernic --backend nvmeof:file=/var/tmp/ns0.img,size=64M
   nvmeof backend: /var/tmp/ns0.img is 1073741824 bytes; size=67108864 \
       would truncate it

Drop ``size=`` to keep the image's size, or give a larger one
to grow it.

Writes land in the host page cache, so the controller reports
a volatile write cache for a file-backed namespace. The guest
then sends Flush when a filesystem or application asks for
its data to be durable (``fsync()``, a journal commit), and
sets Force Unit Access on writes that have to be durable on
their own. The controller completes either only once the data
has reached the image's storage. A shutdown notification
(``CC.SHN``) writes back the whole namespace too, before the
controller reports the shutdown complete; if that fails, the
server logs an error and completes the shutdown anyway. A RAM
namespace has no stable storage, so it reports no write cache
and those requests are no-ops.

Connecting from the Guest
-------------------------

.. code-block:: bash

   sudo modprobe nvme-fabrics nvme-rdma

   # rdma_resolve_addr() needs a neighbour for the controller
   # address; nothing on the wire answers ARP for it.
   sudo ip neigh replace 192.168.200.1 lladdr 02:00:00:00:c0:01 \
       dev rocm-ernic0 nud permanent

   sudo nvme discover -t rdma -a 192.168.200.1 -s 4420
   sudo nvme connect  -t rdma -a 192.168.200.1 -s 4420 -n nvmet-test
   sudo nvme list

   sudo nvme disconnect -n nvmet-test

The neighbour entry is the one unobvious step. The
controller is not a host on the segment and has no MAC of
its own, so the guest's address resolution has nothing to
resolve to. Any link-layer address will do: the emulator
routes the connection by queue pair, not by MAC.

How It Works
------------

Unlike ``loopback`` and ``tcp``, this is not a
transport for guest RDMA traffic -- it is a *peer* that
speaks NVMe-oF back at the guest. It therefore hooks into
the data path that actually serves the guest ``ionic_rdma``
driver (:file:`src/ionic/ionic_datapath_nvmeof.c`), rather
than into the QEMU-derived backend vtable.

Connection establishment
^^^^^^^^^^^^^^^^^^^^^^^^

``nvme connect -t rdma`` goes through ``rdma_cm``, which
means an IB CM exchange over GSI (QP1). The data path
recognises sends on the guest's GSI queue pair and hands the
256-byte MAD to :file:`src/nvmeof/nvmeof_cm.c`, which answers
it:

- **REQ** -- validate the ``cma_hdr`` and the
  ``nvme_rdma_cm_req`` private data behind it, allocate a
  controller-side queue, and reply with **REP** carrying an
  ``nvme_rdma_cm_rep``. An unsupported ``qid`` gets a
  **REJ** with the right NVMe-oF status rather than a
  timeout. The target listens only on its IPv4 ``ip=``
  address, so, as from a Linux target listening on IPv4,
  a connect to any other address, or to any IPv6 address
  (``::ffff:`` mapped ones included), gets a **REJ**
  (invalid service ID). The subsystem NQN is not in the
  rdma_cm private data at all, so a wrong one is caught
  later, by the Fabrics Connect capsule.
- **RTU** -- mark the queue established.
- **DREQ** -- tear the queue down and answer **DREP**. A
  DREQ is matched by its connection ID, or failing that,
  by the controller QPN it names together with the guest's
  connection ID.

Replies are delivered back into the guest's GSI receive
queue as UD completions, with a synthesised 40-byte GRH and
the source MAC and ``IS_IPV4`` bits the upstream driver
requires to report ``IB_WC_GRH | IB_WC_WITH_SMAC``. The
GIDs in that GRH are snooped from the CM REQ, so the
controller never has to be told its own address twice.

Controller queue pairs are numbered from ``0x00c00000``.
The data path spots a send whose destination QPN falls in
that range and routes the capsule to the controller instead
of looking for a fabric peer. The numbers start again from
the bottom of the range once they reach the top, skipping
any a connection still holds.

Capsules and data transfer
^^^^^^^^^^^^^^^^^^^^^^^^^^

Each guest SEND on an established queue carries a 64-byte
NVMe command capsule, which
:file:`src/nvmeof/nvmeof_target.c` executes and answers with
a 16-byte response capsule delivered into a posted receive.

Admin commands cover what ``nvme connect``, ``nvme discover`` and
``nvme list`` need: the fabrics Connect and Property
Get/Set, Identify (controller, namespace, active namespace
list and namespace descriptor list), Get/Set Features, Get
Log Page, Keep Alive, Abort and Async Event. The I/O command
set is Read, Write, Flush, Write Zeroes and Dataset
Management.

Read and Write move their payload with keyed SGLs: the
controller issues RDMA READ or WRITE against the guest's
memory key from inside the data path, using the same
memory-region lookup and guest-memory DMA as the rest of
the ionic data path. ``ioccsz`` is reported as 4 -- a
64-byte capsule with no room after the command -- so the
host always sends a keyed SGL rather than in-capsule data,
and there is exactly one data path to get right.

The key in that SGL comes from fast registration. A stock
``nvme-rdma`` initiator has ``register_always`` on, so it
posts an ``IB_WR_REG_MR`` ahead of every command to bind a
freshly rotated key to the command's pages, and an
``IB_WR_LOCAL_INV`` afterwards. Those arrive as the local
work requests ``IONIC_V1_OP_REG_MR`` and
``IONIC_V1_OP_LOCAL_INV``, which the data path executes
against the same MR table ``CREATE_MR`` fills. Without them
the controller's RDMA READ and WRITE would resolve keys that
name nothing.

A keyed SGL naming key 0 fails with a data transfer error,
and a data block descriptor that is neither keyed nor in
capsule fails with an invalid SGL descriptor type. Key 0 is
the driver's reserved DMA key, which names a raw guest bus
address rather than a registration, and any guest process
that can connect a queue pair to the controller chooses the
key, so honoring it would expose all of guest memory.

Testing
-------

Server-side coverage runs without a VM and is part of
``ctest``:

.. code-block:: bash

   ctest --test-dir build -R nvmeof

- ``nvmeof-target-unit`` -- capsule parsing, the admin and
  I/O command set, and namespace backing.
- ``nvmeof-cm-unit`` -- the CM state machine, including the
  rejection paths.
- ``nvmeof-ci`` -- starts the real server binary once per
  documented option spelling, checks the controller it
  reports, checks that a file-backed namespace appears on
  disk at the right size, and checks that malformed options
  are refused.

The end-to-end connect needs a guest. One Ansible play does
it, by hand or in CI: discover, connect, check
the namespace appears, write 16 MiB of random data through
``O_DIRECT`` and compare digests on read-back, run a short
fio job, disconnect, and check the namespace went away. The
digest comparison is the check that proves the RDMA
transfers behind the capsules actually moved the bytes, and
the disconnect is the only thing that exercises the CM
teardown path at all.

The play needs a single guest already provisioned against an
<<<<<<< Updated upstream
instance running the controller. Run it through ``ci-site.yml``,
which first registers the running guest in the ``ernic_vms``
group; run alone, the play finds no hosts and does nothing:
=======
instance running the controller:
>>>>>>> Stashed changes

.. code-block:: bash

   cd ansible
   ansible-playbook ci-site.yml --tags nvmeof \
     -e ernic_project_root="$PWD/.."

The hosted lane runs the whole thing from scratch --
build, boot a guest, provision it, connect -- in
:file:`.github/workflows/nvmeof-lane.yml`, which is the
cheapest end-to-end RDMA test in the matrix precisely
because it needs only one VM. That lane is a reusable
workflow with a single ``publish`` input, called by
:file:`.github/workflows/system-tests.yml`: false on a pull
request, true on a scheduled or dispatched run against
``develop``. The steps are the same either way, so a green pull
request and a scheduled run mean the same thing.

The controller's address, port and NQN are Ansible
variables (``ernic_nvmeof_traddr``, ``ernic_nvmeof_trsvcid``,
``ernic_nvmeof_nqn`` and friends in
:file:`ansible/group_vars/all.yml`); they have to match the
``--backend`` options the instance was started with.

The play aims the connect at the controller's queue ceiling
rather than leaving it to the initiator, which would otherwise
ask for one queue per online CPU and make the lane's coverage a
property of the runner rather than of the controller. The
ceiling is the ``queues=`` key of the ``--backend`` string, or
eight when the key is absent. The play takes it from
``ernic_nvmeof_queues``, which like the variables above has to
be told what the instance was started with. It then reads
:file:`/sys/class/nvme/nvmeN/queue_count` back and fails unless
it matches, because too few queues is not an error to
``nvme-cli``. A guest with fewer vCPUs than half the ceiling
cannot reach it -- see :ref:`nvmeof-mr-budget` -- so the
expected count is computed from the guest's CPU count, and
falling short of the ceiling is reported rather than failed.

Performance
-----------

The scheduled run of
:file:`.github/workflows/system-tests.yml` sweeps fio
across three block sizes against the connected namespace
and publishes what it measured, on the same terms and
through the same code path as the RDMA and TCP/IP
figures. The numbers land on the
`trend page <https://rocm.github.io/rocm-ernic/perf-trends.html>`_
as the *NVMe-oF read bandwidth over time* chart, and the
4 KiB figure becomes the ``NVMe-oF 4K read`` shield on the
README.

The sweep is random *reads*. A read has the controller
RDMA-WRITE into guest memory, which is the direction worth
a headline number; mixing reads and writes into one figure
would make it mean half of each. The write direction is
covered instead by the digest round trip above, which is a
correctness check rather than a rate.

Three caveats about the numbers, all of which matter more
than the numbers themselves:

- They come from a **GitHub-hosted runner** -- shared
  vCPUs, noisy neighbours, no pinning. Treat them as a
  trend, not a benchmark. Everything published is measured
  on that class of machine now, but these stay in their own
  ``nvmeof`` series: a fio sweep against an in-process
  controller and a perftest sweep across two guests share
  no scale.
- The namespace is RAM by default, so nothing here
  measures storage. What is being measured is the capsule
  and RDMA path: command capsules in by SEND, data out by
  RDMA WRITE, completions back into posted receives.
- Only a green run on ``develop`` publishes. A failed lane's
  numbers describe a broken controller.

To take the same measurements locally, set
``ernic_nvmeof_perf_csv`` to a path and the play writes one
bandwidth CSV there:

.. code-block:: bash

   cd ansible
   ansible-playbook ci-site.yml --tags nvmeof \
     -e ernic_project_root="$PWD/.." \
     -e ernic_nvmeof_perf_csv=/tmp/nvmeof-bw.csv

The block sizes are ``ernic_nvmeof_perf_sweep`` in
:file:`ansible/group_vars/all.yml`. They are not free-form:
they have to stay equal to ``TRACKED_SIZES`` in
:file:`ci/report/publish-perf.py` or the chart quietly drops
a series. Sweep only a guest running under KVM: numbers
measured under TCG are one to two orders of magnitude off, and
neither the play nor the hosted lane checks the accelerator.

Limitations
-----------

- One subsystem with one namespace per server instance.
- No authentication, no discovery-controller persistence,
  no multipath, no reservations.
- No ANA, no namespace management, and no Format NVM.
- Connections do not survive a server restart; a
  file-backed namespace does, but the guest has to
  reconnect.
- Flush, Force Unit Access and a shutdown notification on
  a file-backed namespace wait for the host's writeback in
  the server's main loop, so the device doesn't respond to
  anything else until the data is written.
