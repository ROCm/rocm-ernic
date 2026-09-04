# Changelog for rocm-ernic

## UNRELEASED - rocm-ernic 0.0.1

### Added

* `-L` / `--log-level` and the `ERNIC_LOG_LEVEL` environment variable select
  the server log verbosity (`none`, `error`, `warn`, `info`, `debug`).

### Changed

* The server now defaults to the `warn` log level, so the per-operation `INFO:`
  lines (BAR writes, QP operations, ARP/ICMP packets, TCP mesh events) are
  suppressed in steady state. Use `--log-level info` to restore the previous
  output. `-v` / `--verbose` is now shorthand for `--log-level debug`.

### Removed

### Limitations

### Known issues
