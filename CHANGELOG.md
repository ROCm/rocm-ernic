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

* The server now defaults to the `warn` log level, so the per-operation `INFO:`
  lines (BAR writes, QP operations, ARP/ICMP packets, TCP mesh events) are
  suppressed in steady state. Use `--log-level info` to restore the previous
  output. `-v` / `--verbose` is now shorthand for `--log-level debug`.

### Removed

### Limitations

### Known issues
