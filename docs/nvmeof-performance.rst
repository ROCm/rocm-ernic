NVMe-oF Performance
===================

This page records what the in-process NVMe-oF controller
(:doc:`nvmeof`) actually delivers, measured with ``fio`` from
inside a single guest. It complements :doc:`performance`, which
covers the RDMA verbs path over the TCP mesh, and
:doc:`perf-trends`, which tracks the 4 KiB random-read number
over time from the nightly run.

The numbers here are not storage numbers. Nothing is measured
against a real SSD: the namespace is host memory or a host file,
and every command capsule and RDMA transfer is emulated in
userspace. What the sweep measures is the *emulator* -- the
capsule round trip, the fast-registration path, and the RDMA
READ/WRITE engine in :file:`src/ionic_datapath.c`. Treat a
regression here as a regression in that code, not in any disk.

.. contents:: Sections
   :local:
   :depth: 2

Test Environment
----------------

==============================  ==========================================
Component                       Value
==============================  ==========================================
Host                            hpe-rack-15.adc.amd.com
                                (AMD EPYC 7513, 128 threads)
Host kernel                     6.8.0-124-generic
Guest                           Ubuntu 26.04 LTS, kernel 7.2.3
Guest vCPUs / memory            16 / 32 GiB
Emulated device                 ``1dd8:100a`` (ionic)
Active MTU                      1024 bytes
Backend                         ``nvmeof:size=8G,bs=4096,``
                                ``file=/var/tmp/ernic-ns0.img,queues=4``
Namespace                       8 GiB, file-backed, 4 KiB blocks
fio                             3.41
nvme-cli                        2.16
==============================  ==========================================

The controller reports ``mdts=5`` -- a 128 KiB maximum transfer
-- and ``ioccsz=4``, so every command carries a keyed SGL rather
than in-capsule data. ``maxcmd`` is 128.

Reproducing
-----------

.. code-block:: bash

   # One server, one guest -- a complete fabric.
   ERNIC_INSTANCES=1 \
     ERNIC_BACKEND='nvmeof:size=8G,bs=4096,queues=4' \
     bash ci/jobs/vm-up.sh
   bash ci/jobs/vm-nvmeof.sh

``ci/jobs/vm-nvmeof.sh`` provisions the guest, connects, and runs
the short sweep that feeds the trend chart. The longer sweep
below was driven by hand against the resulting
``/dev/nvme0n1``; each run is ``--direct=1 --time_based
--runtime=15`` with ``libaio``.

Results
-------

Block-size sweep
^^^^^^^^^^^^^^^^

Random read, queue depth 32, 4 jobs.

==========  ==========  ========  ===========  ==========
Block size        IOPS      GB/s    Mean (us)   p99 (us)
==========  ==========  ========  ===========  ==========
4 KiB           43,113     0.177       2913.4     5603.3
16 KiB          45,353     0.743       2748.7     9633.8
64 KiB          41,397     2.713       3055.7     5275.6
256 KiB         15,034     3.941       8345.6    13959.2
1 MiB            3,552     3.725      34829.0    68681.7
==========  ==========  ========  ===========  ==========

IOPS is flat from 4 KiB to 64 KiB: below 64 KiB the cost is
per-capsule, not per-byte, so shrinking the transfer buys
nothing. Bandwidth peaks at 256 KiB and then *falls* at 1 MiB,
which is the ``mdts=5`` cap showing through -- the guest splits
anything over 128 KiB, so a 1 MiB request becomes eight round
trips and pays the per-capsule cost eight times.

Access patterns
^^^^^^^^^^^^^^^

4 KiB, queue depth 32, 4 jobs.

==============  ==========  ========  ===========  ==========
Pattern               IOPS      GB/s    Mean (us)   p99 (us)
==============  ==========  ========  ===========  ==========
Random read         45,283     0.185       2753.2     5472.3
Random write        42,317     0.173       2952.9     8159.2
Random 70/30        41,824     0.171       2991.0     6062.1
==============  ==========  ========  ===========  ==========

Writes land within 7% of reads. The two directions run different
code -- a read makes the controller issue RDMA WRITE into guest
memory, a write makes it issue RDMA READ out of it -- so the
symmetry is the useful result: neither direction carries an
extra copy or an extra round trip the other avoids.

Sequential
^^^^^^^^^^

1 MiB, queue depth 16, 4 jobs.

==========  ==========  ========  ===========  ===========
Pattern           IOPS      GB/s    Mean (us)    p99 (us)
==========  ==========  ========  ===========  ===========
Read             3,398     3.563      18008.4     32374.8
Write            2,359     2.474      26115.1    103284.7
==========  ==========  ========  ===========  ===========

Sequential read reaches roughly the same ceiling as the 256 KiB
random case, confirming the limit is per-transfer overhead
rather than locality. The sequential write p99 of 103 ms is the
file-backed namespace reaching host writeback; a RAM-backed
namespace does not show it.

Queue-depth sweep
^^^^^^^^^^^^^^^^^

4 KiB random read, single job -- the latency curve.

============  ==========  ===========  ==========
Queue depth         IOPS    Mean (us)   p99 (us)
============  ==========  ===========  ==========
1                  2,867        334.3      378.9
2                  7,204        263.6      403.5
4                 11,237        337.5      411.6
8                 20,191        379.0      493.6
16                39,036        394.3      733.2
32                46,042        678.8     1155.1
64                43,713       1445.7     2408.4
============  ==========  ===========  ==========

This is the most diagnostic table on the page. Queue depth 1
gives the unloaded round trip: **334 us** for a 4 KiB read,
covering the capsule SEND, the controller's RDMA WRITE, and the
response capsule. Throughput then scales close to linearly to
depth 16 while latency stays near flat, so the pipeline is
genuinely parallel rather than serialised behind one lock.

Saturation is at depth 32, around **46,000 IOPS**. Past that the
curve inverts: depth 64 delivers *fewer* IOPS at twice the
latency, so 32 is the useful operating point and deeper queues
only add waiting.

Data integrity
^^^^^^^^^^^^^^

A ``--verify=crc32c --verify_fatal=1`` pass over 512 MiB of
4 KiB random writes completed with **zero** verification
errors, at 28,519 write IOPS. Combined with the digest round
trip in ``ansible/playbooks/nvmeof-tests.yml``, this is the
check that the keyed-SGL path moves the right bytes and not
merely the right number of bytes.

Known Limits
------------

Sub-block I/O is rejected
^^^^^^^^^^^^^^^^^^^^^^^^^

With the namespace formatted at 4 KiB, 512-byte and 1 KiB
requests fail::

   fio: io_u error on file /dev/nvme0n1: Invalid argument:
        read offset=2098144256, buflen=512

That is correct behaviour, not a defect -- the block layer
refuses I/O below the logical block size. Use ``bs=512`` on the
backend to measure the small-block path.

.. _nvmeof-mr-budget:

The MR budget caps the queue count
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This is the one to know about before changing ``queues=``.

A stock ``nvme-rdma`` initiator runs with ``register_always``,
and allocates a pool of **128 memory regions per queue** at
connect time -- admin queue included -- rather than per I/O. An
N-queue controller therefore needs ``(N + 1) * 128`` regions
before the first capsule moves, and the emulator's table is a
fixed ``IONIC_MAX_MR`` = 2048 (:file:`src/ionic_datapath.h`,
which also sizes ``MAX_MR`` in
:file:`third-party/qemu/hw/rdma/rdma_rm_defs.h` and in
:file:`src/ionic_datapath.c`). That puts the ceiling at 15 I/O
queues, which ``nvmeof_parse_backend()`` enforces at startup:

.. code-block:: text

   Error: nvmeof backend: queues must be 1..15, one per 128
   memory regions in a table of 2048 (got '16')

The table is not sized for the NVMe maximum of 64 queues on
purpose: ``dp_reg_mr()`` walks the whole table on every fast
registration -- that is, on every command -- so the constant is
a per-I/O cost as well as a ceiling.

The measurements above predate that change and were taken at
``queues=4`` against the old 1024-entry table. They have not been
rerun. The change does not alter protocol behaviour once a connect
has succeeded, but it is not free either: ``dp_reg_mr()`` walks the
whole table on every fast registration, as above, so doubling the
constant doubles that walk. Treat the figures as a baseline for the
1024-entry table rather than as current numbers.

Both CI lanes now aim the connect at the controller's ceiling
rather than letting the initiator choose. ``nvme-rdma`` defaults
``nr_io_queues`` to ``num_online_cpus()``, so before the change
the hosted lane -- a 4-vCPU guest -- negotiated four queues and
needed only 544 regions, comfortably inside even the old
1024-entry table. That is why the lane stayed green through the
first failure described below and never covered it. It did not
escape the second: 544 regions overran the 256-entry handle map,
so the four-queue lane was leaking regions silently the whole
time. The connect sites therefore pass ``-i`` *and* ``-W``:
``nvmf_nr_io_queues()`` is the sum of ``nr_io_queues``,
``nr_write_queues`` and ``nr_poll_queues``, each separately
clamped to ``num_online_cpus()``, and ``nvmf_parse_options()``
clamps ``-i`` to the CPU count a second time on its own, so
``-i 8`` alone cannot lift a 4-vCPU guest past four. Eight of
each reaches eight there, and on a larger guest the controller's
Set Features NUMBER_OF_QUEUES reply brings the total back down
to eight. Both lanes then read
:file:`/sys/class/nvme/nvmeN/queue_count` back and fail if it
does not match, because too few queues is not an error to
``nvme-cli`` and the shortfall would otherwise be invisible.

What they match against is ``min(2 * min(C, cpus), C) + 1``,
where ``C`` is the ``queues=`` the instance was started with,
not a hard-coded nine: at the same ``queues=4`` this page was
measured at the lane should expect five, and a guest with fewer
than ``C / 2`` vCPUs cannot reach ``C`` however the flags are
spent. Coming up short is legitimate and is logged rather than
failed -- but it does mean that run is not covering the full
memory-region budget.

One correction to the arithmetic above, in the safe direction:
the pool is sized ``queue->queue_size``, and the admin queue's
is ``NVME_AQ_DEPTH`` = 32, not 128. The real demand is
``N * 128 + 32``, so ``(N + 1) * 128`` over-counts by 96 and the
2048-entry table and its 15-queue ceiling remain sound.

Historical note
"""""""""""""""

Before the memory-region budget was reworked, ``MAX_MR`` was
1024 -- exactly the 8 * 128 regions the default ``queues=8`` needs for
its I/O queues, leaving nothing for the admin queue. The connect
died on the last one::

   nvme nvme0: creating 8 I/O queues.
   ionic 0000:01:00.0 rocm-rdma-ernic0: opcode 3 error 16777216
   nvme nvme0: failed to initialize MR pool sized 128 for QID 8
   nvme nvme0: rdma connection establishment failed (-22)

Compounding it, the admin queue's driver-id-to-handle map held
only 256 entries, so every region past the 256th was created
successfully and then had no map entry. ``CREATE_MR`` still
reported success, ``DESTROY_MR`` could not resolve the handle
and silently returned 0, and the regions leaked for the life of
the process -- which is why a second connect attempt failed
earlier than the first, at QID 2 rather than QID 8.
