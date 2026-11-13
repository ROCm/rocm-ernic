NVMe Key Value Performance
==========================

This page records what the Key Value command set delivers on a
controller that serves a block namespace and a Key Value namespace at
the same time (:doc:`nvmeof`). It complements
:doc:`nvmeof-performance`, which measures the block namespace with
``fio``, and :doc:`perf-trends`, which tracks the 64 KiB Retrieve
number over time from the nightly run.

As with the block page, these are not storage numbers. The namespace
is a hash table in the server's own memory and every command capsule
and RDMA transfer is emulated in userspace, so what the sweep measures
is the emulator: the capsule round trip, the key decode, and the RDMA
engine in :file:`src/ionic/ionic_datapath.c`. Treat a regression here
as a regression in that code.

.. contents:: Sections
   :local:
   :depth: 2

What the numbers are, and are not
---------------------------------

**This is a depth-one synchronous path.** :file:`tests/kv_bench.c`
issues one command, waits for it, and issues the next, through
``ioctl(NVME_IOCTL_IO_CMD)``. That is the right shape for a latency
figure and for catching a regression in the per-command cost, and it
is the wrong shape for a throughput headline: a queued initiator over
``io_uring`` passthrough reaches an order of magnitude more bandwidth
on the same controller, because it keeps many commands outstanding
rather than paying the round trip serially.

So read the bandwidth columns as *size divided by a fixed per-command
cost*, which is exactly what they are.

**It is not a loop around nvme-cli.** ``nvme io-passthru`` costs about
5 ms per invocation to fork, exec and dynamically link, which is more
than an order of magnitude above the command it issues. Timing a shell
loop around it reports the same ~5 ms at every value size: a flat line
that looks like a working measurement and tracks nothing about the
device. That is why the lane builds a small benchmark in the guest
instead.

Test Environment
----------------

==============================  ==========================================
Component                       Value
==============================  ==========================================
Host                            AMD EPYC 9655, 32 threads available
Host kernel                     6.8.0-117-generic
Guest                           Ubuntu, kernel 7.2.6-070206-generic
Guest vCPUs / memory            8 / 16 GiB
Emulated device                 ``1dd8:1002`` (ionic)
Active MTU                      1024 bytes
Backend                         ``nvmeof:size=1G,bs=4096,``
                                ``ns=2,kv=1,size=2G,keys=8192,maxval=1M``
Namespaces                      nsid 1: 1 GiB block, 4 KiB blocks

                                nsid 2: Key Value, 2 GiB, 8192 keys
nvme-cli                        2.16
==============================  ==========================================

The controller reports ``mdts=5``, ``maxcmd=128`` and ``ioccsz=4``, so
every command carries a keyed SGL rather than in-capsule data.

Results
-------

Four runs of 64 iterations each, median reported. ``kv_bench`` Stores,
Retrieves, compares the bytes, and only then times each direction.

============  ==============  =================  =============  ================
Value size    Store (GB/s)    Retrieve (GB/s)    Store (µs)     Retrieve (µs)
============  ==============  =================  =============  ================
4 KiB         0.012           0.012              330            331
64 KiB        0.244           0.199              230            330
128 KiB       0.549           0.398              235            328
============  ==============  =================  =============  ================

**Retrieve latency is flat.** 331, 330 and 328 µs across a 32-fold
change in value size: the transfer itself is not what the time is
spent on, and the bandwidth column is therefore the value size divided
by an almost constant per-command cost. A regression in that fixed
cost is the thing this lane is positioned to catch, and it will show
up in every row at once rather than at one size.

**Store is faster than Retrieve above 4 KiB, and noisier.** Store
ranged 0.199 to 0.292 GB/s at 64 KiB and 0.454 to 0.588 at 128 KiB
across the four runs, against a Retrieve that held within a few per
cent. The two directions are not symmetric: a Store is a
host-to-controller transfer the target copies into the store, and a
Retrieve is the other direction. They are charted separately for that
reason -- one averaged number would hide a regression in whichever
direction did not move.

**1 MiB is absent by construction.** The controller reports MDTS 5, so
one Key Value command carries at most ``2^5 * 4 KiB`` = 128 KiB
however large ``maxval`` is, and the kernel refuses a longer transfer
before the device sees it. 128 KiB is the ceiling, not a choice. That
is also why the trend chart draws only two of the three tracked sizes
for this lane.

What the lane also checks
-------------------------

The sweep is the last thing the play does. Before it, and on every run
rather than only when publishing:

* Both namespaces attach, and each reports its **own** command set --
  CSI 0 for the block namespace and CSI 1 for the Key Value one. Both
  have to say so explicitly: once a controller advertises more than
  one command set the host stops assuming NVM, and a block namespace
  that reports nothing is dropped with ``Command set not reported for
  nsid``.

* A full Key Value round trip: Exist on an empty store misses, Store,
  Exist hits, Retrieve returns the same bytes, Delete, Exist misses
  again.

* **Isolation, which is the point of the lane.** A pattern is written
  to the block namespace before any Key Value traffic and digested
  afterwards. A Store is opcode 01h and a block Write is opcode 01h;
  the only thing that tells them apart is the namespace the command
  names. A controller that dispatched on itself rather than on the
  addressed namespace would pass every check above and fail this one.

* A Key Value opcode sent to the block namespace is refused. ``Exist``
  (0x14) has no meaning in the NVM command set, so running it rather
  than rejecting it would say dispatch is not per namespace.

Reproducing
-----------

One server and one guest are a complete fabric, for Key Value exactly
as for blocks. Start the server on a mixed backend:

.. code-block:: bash

   rocm-ernic --socket /tmp/vfio-user-rocm-ernic.sock \
       --backend "nvmeof:size=1G,bs=4096,ip=192.168.200.1,\
   ns=2,kv=1,size=2G,keys=8192,maxval=1M"

``ns=2`` opens a second namespace numbered 2; the options after it
describe that one. Then, from the guest, after
:doc:`testing`'s guest setup has run:

.. code-block:: bash

   cd ansible
   ansible-playbook ci-site.yml --tags nvmeof-kv \
       -e ernic_run_dir=/tmp/ernic-ci \
       -e ernic_vm_ssh_base_port=2222 \
       -e ernic_instances=1 \
       -e ernic_kv_perf_csv=/tmp/results/perf-csv/nvmekv-bw.csv

Leave ``ernic_kv_perf_csv`` unset to run the functional checks without
the sweep, which is what a pull request does.

To run just the benchmark by hand, against the generic char device of
the Key Value namespace:

.. code-block:: bash

   gcc -std=c11 -pedantic -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
       -O2 -Isrc -o kv_bench tests/kv_bench.c
   sudo ./kv_bench /dev/ng0n2 2 64 4096 65536 131072

It opens only a generic char device, named ``/dev/ng<ctrl>n<ns>`` as
Linux names it, but can't tell whether it is the Key Value namespace's:
a block namespace has one too, and runs a Store as a Write.

Note that the device name is **not** derived from the namespace id.
Linux names namespace devices by the head instance it allocated, and a
hidden Key Value namespace still consumes one, so nsid 2 is commonly
``ng0n1`` and nsid 1 is ``nvme0n2``. Resolve it through sysfs:

.. code-block:: bash

   for d in /sys/class/nvme/nvme0/nvme0n*; do
       echo "$(basename "$d") -> nsid $(cat "$d/nsid")"
   done
