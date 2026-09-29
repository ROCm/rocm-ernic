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

5. **VM Metrics** -- guest-side panels for VM CPU,
   memory and disk I/O, TCP/IP rates on the
   ``rocm-ernic0`` and management interfaces, RDMA port
   data and packet rates, and GPU temperature,
   utilisation, power, clock and PCIe bandwidth.

   Unlike rows 1-4, this row does not read from
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

Alert when any instance is down:

.. code-block:: promql

   ernic_instance_up == 0

Alert on FLR reset:

.. code-block:: promql

   increase(ernic_flr_reset_total[5m]) > 0
