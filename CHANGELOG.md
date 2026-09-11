# Changelog for rocm-ernic

## UNRELEASED - rocm-ernic 0.0.1

### Added

* `-L` / `--log-level` and the `ERNIC_LOG_LEVEL` environment variable select
  the server log verbosity (`none`, `error`, `warn`, `info`, `debug`).
* `-I` / `--ionic` presents the device as an AMD Pensando ionic NIC
  (`1022:8001`) so the guest can use the upstream Linux `ionic` and
  `ionic_rdma` drivers instead of the companion module in `driver/`. The
  guest-side patches live in `patches/` and are built into a DKMS package by
  the `ERNIC_BUILD_KMOD` targets.
* `-T` / `--tap IFNAME` attaches the emulated Ethernet interface to a host TAP
  in ionic mode, giving the guest a routable Ethernet segment with working
  ARP, ICMP, and TCP/IP.
* `tests/test_ionic_ci.sh` (CTest `ionic-ci`) covers the ionic device
  identity, BAR geometry, shutdown, and `--tap` handling without needing a VM.
* CI job `ionic-patches` verifies that every patch in `patches/` still applies
  to the pinned `IONIC_KERNEL_REF`.

### Changed

* ionic is now the default device personality. `rocm-ernic` with no flag
  presents `1022:8001` and the guest runs the upstream `ionic` and
  `ionic_rdma` drivers with the `patches/` applied, plus the upstream
  `providers/ionic` in rdma-core. `-I` / `--ionic` is still accepted and
  does nothing. The systemd service, `ernicctl` and the
  `sbates130272.rocm_ernic` Ansible collection follow the same default via
  `ERNIC_DEVICE_MODE` and `ernic_device_mode`.
* In ionic mode guest Ethernet leaves through a host TAP, so each instance
  needs its own TAP enslaved to a shared bridge for guest-to-guest IP. The
  `ernic_host_setup` role creates them from `ERNIC_TAP_PREFIX` /
  `ERNIC_TAP_BRIDGE`, and `ci/runner/install-runner.sh` does the same
  one-time root step for the self-hosted CI node.
* The self-hosted CI runs ionic by default, with a `mode` dispatch input for
  the legacy path. Legacy runs are never published to the performance trend
  series, and `ci/report/publish-perf.py` records the mode of each run and
  refuses to mix modes in one series without `--allow-mode-change`.
* rocm-xio is built against the ionic provider (`-DGDA_IONIC=ON
  -DRDMA_CORE_BUILD=OFF`) with the `rdma-core/ionic-gda` patches applied to
  the guest's rdma-core, rather than the `rocm_ernic` GDA backend.
* The server now defaults to the `warn` log level, so the per-operation `INFO:`
  lines (BAR writes, QP operations, ARP/ICMP packets, TCP mesh events) are
  suppressed in steady state. Use `--log-level info` to restore the previous
  output. `-v` / `--verbose` is now shorthand for `--log-level debug`.

### Deprecated

* The out-of-tree `rocm_ernic` driver in `driver/` and its PVRDMA-derived
  device (`1022:8000`). Select it with `--legacy` (alias `--pvrdma`),
  `ERNIC_DEVICE_MODE=legacy` or `-e ernic_device_mode=legacy`; the server
  prints a warning when it is used. Nothing has been removed, and the path
  is still covered by CI, but new deployments should use ionic.

### Removed

### Limitations

### Known issues
