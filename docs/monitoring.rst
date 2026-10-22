Prometheus Monitoring and Grafana Dashboard
============================================

rocm-ernic ships a Prometheus exporter
(``ernic-exporter``) that reads the same data sources
as ``ernicctl`` -- the ``instances.json`` manifest and
per-instance ``*.stats`` files under ``ERNIC_RUN_DIR``
-- and exposes them as Prometheus metrics on an HTTP
endpoint.  A pre-built Grafana dashboard is included
for visualising cluster state, network traffic, RDMA
vs TCP/IP ratios, FLR events, and VM lifecycle.

.. contents:: On this page
   :local:
   :depth: 2

Architecture
------------

.. code-block:: text

   ┌─────────────────── Host ───────────────────────┐
   │                                                 │
   │  rocm-ernic         *.stats (text, ~1 Hz)       │
   │  server instances ──────────────► ernic-exporter │
   │        │                           :9840/metrics │
   │        └──► instances.json ──────►     │         │
   │                                        │         │
   └────────────────────────────────────────┼─────────┘
                                            │
                                    Prometheus scrape
                                            │
                                       ┌────▼────┐
                                       │ Grafana │
                                       └─────────┘

The exporter runs as a ``systemd`` service alongside
``rocm-ernic``.  Prometheus scrapes ``/metrics`` at a
configurable interval (default 5 s); each scrape reads
the current stat files on-demand so no internal polling
loop is needed.

Quick Start
-----------

**Manual (single host):**

.. code-block:: bash

   # 1. Install the exporter's Python dependency
   python3 -m venv /opt/ernic-exporter-venv
   /opt/ernic-exporter-venv/bin/pip install \
       -r /usr/share/rocm-ernic/requirements-exporter.txt

   # 2. Start the exporter (uses system Python if
   #    prometheus_client is installed system-wide,
   #    otherwise point the unit at the venv -- see
   #    Ansible section below)
   sudo systemctl daemon-reload
   sudo systemctl enable --now ernic-exporter

   # 3. Verify
   curl -s http://localhost:9840/metrics | head -20

.. note::

   ``ansible/`` used to carry a ``monitoring-setup.yml``
   play that did the above, plus a Prometheus ``file_sd``
   scrape config and the Grafana dashboard JSON. It was
   removed in 0.2.0 along with the rest of the automation
   that was not on the guest-setup path. The manual steps
   above are the supported route.

Configuration
-------------

All settings come from the same
``/etc/rocm-ernic/rocm-ernic.env`` file that the main
service uses, plus two exporter-specific variables:

.. list-table::
   :header-rows: 1
   :widths: 30 15 55

   * - Variable
     - Default
     - Description
   * - ``ERNIC_RUN_DIR``
     - ``/run/rocm-ernic``
     - Directory containing ``instances.json`` and
       ``*.stats`` files.
   * - ``ERNIC_EXPORTER_PORT``
     - ``9840``
     - HTTP port for the ``/metrics`` endpoint.

Override via environment or in the env file:

.. code-block:: bash

   echo "ERNIC_EXPORTER_PORT=9841" | \
       sudo tee -a /etc/rocm-ernic/rocm-ernic.env
   sudo systemctl restart ernic-exporter

Ansible variables (``group_vars/all.yml``):

.. list-table::
   :header-rows: 1
   :widths: 35 20 45

   * - Variable
     - Default
     - Description
   * - ``ernic_monitoring``
     - ``true``
     - Feature gate; set ``false`` to skip
       monitoring deployment entirely.
   * - ``ernic_exporter_port``
     - ``9840``
     - Maps to ``ERNIC_EXPORTER_PORT``.
   * - ``ernic_exporter_venv``
     - ``/opt/ernic-exporter-venv``
     - Path for the Python venv.
   * - ``ernic_exporter_scrape_interval``
     - ``5s``
     - Prometheus scrape interval.
   * - ``ernic_prometheus_config_dir``
     - ``/etc/prometheus/file_sd``
     - Directory for Prometheus ``file_sd`` configs.
   * - ``ernic_grafana_dashboard_dir``
     - ``/var/lib/grafana/dashboards``
     - Grafana provisioning dashboards directory.

Metrics Reference
-----------------

All metric names start with ``ernic_``.  Labels use
``ernic_id`` for the numeric server instance ID (1, 2,
...), ``role`` for the instance role, and ``qp`` for
the QP handle.

.. note::

   The instance label is ``ernic_id``, not ``instance``.
   Prometheus reserves ``instance`` for the scrape
   target (``host:port``), so every exporter metric
   would collide on it.  Nearly all per-instance
   metrics carry ``role`` as well, and the shipped
   dashboard filters on both.

Every metric whose name ends in ``_total`` is a counter
-- a value that only ever climbs, until the server
restarts and it resets to zero.  Query those with
``rate()`` or ``increase()``, never as a raw value.
Everything else is a gauge and can be read directly.

.. note::

   The exporter reads absolute totals out of the
   ``*.stats`` files, so it builds its counters through
   a custom collector.  ``prometheus_client``'s own
   ``Counter`` class only offers ``inc()``, which cannot
   express "the server says the total is now N".

Cluster and Instance Metrics
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 38 10 52

   * - Metric
     - Type
     - Description
   * - ``ernic_instances``
     - Gauge
     - Number of rocm-ernic server instances.
   * - ``ernic_instance_up``
     - Gauge
     - Whether each instance process is alive
       (1 = running, 0 = dead).
       Labels: ``ernic_id``, ``role``, ``mac``.
   * - ``ernic_instance_uptime_seconds``
     - Gauge
     - Process uptime in seconds.
       Labels: ``ernic_id``, ``role``.

VM Lifecycle Metrics
^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 38 10 52

   * - Metric
     - Type
     - Description
   * - ``ernic_vms``
     - Gauge
     - Number of attached VMs.
   * - ``ernic_vm_attached``
     - Gauge
     - Whether a VM is attached (1 = yes).
       Labels: ``ernic_id``, ``role``, ``vm_name``.
   * - ``ernic_vm_uptime_seconds``
     - Gauge
     - VM uptime in seconds.
       Labels: ``ernic_id``, ``role``.
   * - ``ernic_vm_gpu_passthrough``
     - Gauge
     - Whether GPU passthrough is enabled (1 = yes).
       Labels: ``ernic_id``, ``role``.

Network Traffic
^^^^^^^^^^^^^^^

All per-instance, labelled
``{ernic_id="<id>", role="<role>"}``:

.. list-table::
   :header-rows: 1
   :widths: 38 10 52

   * - Metric
     - Type
     - Description
   * - ``ernic_ip_bytes_tx_total``
     - Counter
     - Total IP/Ethernet bytes transmitted.
   * - ``ernic_ip_bytes_rx_total``
     - Counter
     - Total IP/Ethernet bytes received.
   * - ``ernic_rdma_bytes_sent_total``
     - Counter
     - Total bytes sent via RDMA SEND operations.
   * - ``ernic_rdma_bytes_received_total``
     - Counter
     - Total bytes received via RDMA RECV operations.
   * - ``ernic_rdma_bytes_read_total``
     - Counter
     - Total bytes via RDMA Read.
   * - ``ernic_rdma_bytes_write_total``
     - Counter
     - Total bytes via RDMA Write.
   * - ``ernic_rdma_bytes_total``
     - Counter
     - Aggregate: send + recv + read + write.
   * - ``ernic_ip_bytes_total``
     - Counter
     - Aggregate: IP TX + RX.

Device Health and Events
^^^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 38 10 52

   * - Metric
     - Type
     - Description
   * - ``ernic_flr_reset_total``
     - Counter
     - Cumulative FLR / device reset count.
       Labels: ``ernic_id``, ``role``.
   * - ``ernic_commands_total``
     - Counter
     - Total admin queue commands processed.
       Labels: ``ernic_id``, ``role``.
   * - ``ernic_interrupts_total``
     - Counter
     - Total interrupts delivered.
       Labels: ``ernic_id``, ``role``.
   * - ``ernic_connection_up``
     - Gauge
     - Connection state (1 = connected).
       Labels: ``ernic_id``, ``role``, ``state``.
   * - ``ernic_mmio_reads_total``
     - Counter
     - Total MMIO read operations.
       Labels: ``ernic_id``, ``role``.
   * - ``ernic_mmio_writes_total``
     - Counter
     - Total MMIO write operations.
       Labels: ``ernic_id``, ``role``.
   * - ``ernic_stats_writes_total``
     - Counter
     - How many times the server has flushed stats.
       Labels: ``ernic_id``, ``role``.

Per-QP Metrics
^^^^^^^^^^^^^^

All labelled
``{ernic_id="<id>", role="<role>", qp="<handle>"}``
except ``ernic_qp_count``, which has no ``qp`` label:

.. list-table::
   :header-rows: 1
   :widths: 38 10 52

   * - Metric
     - Type
     - Description
   * - ``ernic_qp_count``
     - Gauge
     - Number of active Queue Pairs.
       Labels: ``ernic_id``, ``role`` (no ``qp``).
   * - ``ernic_qp_bytes_sent_total``
     - Counter
     - Bytes sent via SEND on this QP.
   * - ``ernic_qp_bytes_received_total``
     - Counter
     - Bytes received via RECV on this QP.
   * - ``ernic_qp_bytes_rdma_read_total``
     - Counter
     - RDMA Read bytes on this QP.
   * - ``ernic_qp_bytes_rdma_write_total``
     - Counter
     - RDMA Write bytes on this QP.
   * - ``ernic_qp_wqes_processed_total``
     - Counter
     - Total WQEs processed on this QP.
   * - ``ernic_qp_cqes_posted_total``
     - Counter
     - Total CQEs posted on this QP.
   * - ``ernic_qp_doorbell_send_total``
     - Counter
     - Send doorbell rings on this QP.
   * - ``ernic_qp_doorbell_recv_total``
     - Counter
     - Receive doorbell rings on this QP.

LIF Counters
^^^^^^^^^^^^

The emulated NIC keeps the ionic LIF statistics block,
the 1 KiB ``struct ionic_lif_stats`` it writes into
guest memory and the guest reads for ``ip -s link``.
This is a different counter set from the guest's RDMA
``hw_counters``, which the device serves from the admin
queue; the next section covers those.  The exporter
publishes the whole block as a single metric, with the
counter in a label:

.. list-table::
   :header-rows: 1
   :widths: 38 10 52

   * - Metric
     - Type
     - Description
   * - ``ernic_lif_counter_total``
     - Counter
     - One LIF statistics counter, under its
       ``ionic_if.h`` name.
       Labels: ``ernic_id``, ``role``, ``counter``.

This is one metric with a ``counter`` label rather than
one metric per counter because the set belongs to the
device, not to the exporter: a counter the emulator
starts keeping is published the moment the server
writes it, with no exporter release.  The guest's
``ionic_rdma`` driver makes the same bargain -- it takes
its counter names from a table the device hands back.

The label values are exactly the field names of
``struct ionic_lif_stats``, so a counter can be looked
up in the kernel's ``ionic_if.h``.

A counter the emulator does not yet keep reads zero
rather than disappearing; the server prints the whole
block on every flush.

.. include:: generated/lif-counters.rst

.. note::

   The ``rdma_req_*`` and ``rdma_resp_*`` error counters
   are named and published, but nothing in the datapath
   increments them yet, so they read zero whatever
   happens.  The Ethernet, drop, ring and RDMA traffic
   counters are live, and
   ``tests/py/test_ionic_hw_stats.py`` drives traffic
   that moves them.

Guest hw_counters and real silicon
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

A guest running the in-tree ``ionic_rdma`` driver gets
its own view of the device's counters, independent of
the exporter, under
``/sys/class/infiniband/<dev>/ports/1/hw_counters``:

.. code-block:: bash

   rdma statistic show link ionic_0/1
   rdma statistic qp set link ionic_0/1 auto type on
   rdma statistic qp show

Those come from four admin commands --
``STATS_HDRS`` and ``QP_STATS_HDRS`` hand back a table
naming the counters, ``STATS_VALS`` and
``QP_STATS_VALS`` fill in the values.  The driver takes
the names, value types and offsets from that table, so
the device decides what a guest sees.

The values behind those tables are the server's own
device and per-QP counters, not the LIF block, and the
exporter has always published them -- under different
names.  What a guest reads as ``total_bytes_sent`` is
``ernic_rdma_bytes_sent_total`` here, and what it reads
per QP as ``wqes_processed`` is
``ernic_qp_wqes_processed_total``.  The tables are
defined in ``dev_stats`` and ``qp_stats`` in
``src/ionic_adminq_priv.h``:

.. include:: generated/hw-counter-tables.rst

So the device exports two counter sets, and they stand
differently against the silicon the emulator models:

* The **LIF statistics block** matches.  All 74 named
  counters are at the qword offsets, and under the
  names, that ``struct ionic_lif_stats`` gives them in
  the kernel's ``ionic_if.h``, which is the layout the
  Pensando DSC firmware maintains.  A tool that reads
  ``ip -s link`` or the LIF block directly sees what it
  would see on hardware.
* The **hw_counters tables** do not.  The device and
  per-QP counters the emulator advertises
  (``commands``, ``total_bytes_sent``,
  ``doorbell_send`` and the rest) are its own: they
  describe the emulator's accounting, and the real
  firmware's names for its tables are not published in
  the kernel tree or anywhere else we can check.  The
  wire format is right and the driver is happy, but a
  dashboard keyed on these names is keyed on rocm-ernic,
  not on the DSC.

This is a naming difference, not a protocol one.  The
ABI is firmware-defined on both sides: the driver never
assumes a counter set, so the emulator is free to
advertise its own, and there is no guest-visible
breakage either way.  Anyone with DSC hardware can
settle the question by comparing the output of
``rdma statistic show`` there with ``dev_stats``.

Grafana Dashboard
-----------------

The dashboard JSON is at
``prometheus/grafana/ernic-dashboard.json`` in the
source tree and
is installed to the Grafana provisioning directory by
the Ansible playbook.  It can also be imported manually
via the Grafana UI (Dashboards > Import > Upload JSON).

Dashboard rows:

1. **Cluster Overview** -- stat panels for instance
   count, running instances, attached VMs, FLR resets,
   total QPs, and GPU-passthrough VMs; tables showing
   server and VM details.

2. **Network Traffic** -- time-series panels for IP and
   RDMA traffic rates; a pie chart comparing RDMA vs
   IP total bytes; a per-server traffic totals table.

3. **RDMA Detail** -- separate Send/Recv and Read/Write
   rate panels; a per-QP traffic table.

4. **Server Health** -- FLR reset count over time,
   commands/s, interrupts/s, MMIO read/write rates,
   and stats write count.

5. **LIF Counters (ionic_lif_stats)** -- the ionic
   LIF statistics block: RDMA packet rate, RDMA error
   rate, Ethernet drop and ring-error rate, and a table
   of every counter that has moved.  The two rate
   panels filter out flat-zero series, so a line
   appearing is itself the signal.

6. **VM Metrics** -- guest-side panels for VM CPU,
   memory and disk I/O, TCP/IP rates on the
   ``rocm-ernic0`` and management interfaces, RDMA port
   data and packet rates, and GPU temperature,
   utilisation, power, clock and PCIe bandwidth.

   Unlike rows 1-5, this row does not read from
   ``ernic-exporter``.  Its panels query ``node_*``,
   ``rdma_port_*`` and ``gpu_*`` series from separate
   ``ernic-vm-node``, ``ernic-vm-rdma`` and
   ``ernic-vm-gpu`` scrape jobs that run inside the
   guest, so it stays empty unless those are
   configured.

The dashboard defines three template variables:
``$instance`` filters by ``ernic_id``, ``$role`` by
instance role, and ``$disk_device`` selects the guest
block device for the VM disk panel.  Every
``ernic-exporter`` panel filters on both ``$instance``
and ``$role``, so leaving ``$role`` unset hides all
data.  Default refresh is 10 s with a 1 h time window.

Useful PromQL Examples
----------------------

Aggregate RDMA throughput across all instances:

.. code-block:: promql

   sum(rate(ernic_rdma_bytes_total[5m]))

Per-instance IP vs RDMA ratio:

.. code-block:: promql

     rate(ernic_rdma_bytes_total[5m])
   / (rate(ernic_rdma_bytes_total[5m])
      + rate(ernic_ip_bytes_total[5m]))

Any RDMA error the device reported, across the
cluster:

.. code-block:: promql

   sum by (counter) (
     rate(ernic_lif_counter_total{
       counter=~"rdma_(req|resp)_.*"}[5m])
   ) > 0

One LIF counter by name, as a guest would read it from
``hw_counters``:

.. code-block:: promql

   ernic_lif_counter_total{counter="tx_rdma_ucast_bytes"}

Frames the emulated datapath dropped:

.. code-block:: promql

   sum by (ernic_id) (
     rate(ernic_lif_counter_total{
       counter=~".*_drop_packets"}[5m])
   )

Alert when any instance is down:

.. code-block:: promql

   ernic_instance_up == 0

Alert on FLR reset:

.. code-block:: promql

   increase(ernic_flr_reset_total[5m]) > 0
