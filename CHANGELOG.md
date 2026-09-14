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

### Fixed

* Memory region bounds checks in the TCP and loopback backends no longer
  overflow. The checks were written as `addr + len > start + length`, whose
  unchecked 64-bit arithmetic a peer could wrap by choosing an `addr` just
  below the region modulo 2^64: the comparison then succeeded and the host
  pointer computed from it landed an attacker-chosen distance outside the
  registered region, giving an out-of-bounds read or write. Both backends now
  test containment by subtraction, so every intermediate stays inside a range
  already proven not to wrap. This covers the five memory-region bounds checks:
  the TCP `RDMA_WRITE` and `RDMA_READ_REQ` handlers, `tcp_wr_map_sge()`, the
  local loopback path in `tcp_post_send()`, and `loopback_translate_addr()`.
  It does not cover every unchecked sum in the datapath -- see "Known issues"
  below. Both backends take both of their bounds from the same guest-supplied
  registration -- `cmd->start` becomes `mr->start` in `rdma_rm_alloc_mr()` and
  `lmr->guest_start` in the loopback `create_mr()` -- so `start + length` was a
  live wrap point on both sides, not just one.
* The local loopback path in `tcp_post_send()` now accumulates scatter-gather
  lengths in 64 bits. A sufficiently long SGE list could wrap the previous
  32-bit sum, which then passed the bounds check while the per-entry copies
  still wrote their full unwrapped lengths.
* The TCP backend no longer forms host pointers from memory regions that have
  no host mapping. `rdma_rm_alloc_mr()` records `virt = NULL` when the mapping
  fails and `create_mr()` continues anyway, so the region kept the guest's own
  `start` and `length`. The bounds check was satisfiable by construction and
  the offset was then added to `NULL`, turning an out-of-region write into an
  absolute write at a guest-chosen address. The region's backing is now part
  of the bounds check, alongside its extent.
* The remote send path in `tcp_post_send()` now sums scatter-gather lengths in
  64 bits and bounds the result before it reaches the wire. The previous
  32-bit sum sized two heap buffers whose fill loops wrote the full unwrapped
  per-entry lengths, so a wrapped total undersized the allocation rather than
  merely mis-reporting it. The receive path enforces its ceiling per message
  rather than per send, so the bound is applied per branch, against whatever
  that branch actually frames as one message: an RDMA write sends the whole
  sum behind a header, so its total is capped one header below the ceiling; a
  read request is header-only and its total describes the peer's response, so
  it is capped at the ceiling itself; a large send emits one message per
  scatter-gather entry, so each entry is capped and the total is left alone.
  A single bound on the total would have rejected large multi-entry sends that
  the wire format handles without difficulty. A static assertion ties the
  coalesced branch's threshold and header to the ceiling so that relationship
  cannot drift.
* `pvrdma_map_to_pdir()` no longer truncates its page-count check. `nchunks *
  PAGE_SIZE` was computed in 32-bit while the length it is compared against is
  64-bit, so a guest could wrap the product to match a short length and then
  drive the page walk far past the mapping it had just sized. The product is
  now widened, and the walk's own page offsets along with it.
* `pvrdma_map_to_pdir()` now bounds the page count itself, which nothing did
  before: `create_mr()` passed the guest's `nchunks` straight through, unlike
  the completion-queue, queue-pair and shared-receive-queue paths, which all
  bound theirs. The walk indexes the page directory once per 512 chunks, but
  the directory is a single mapped page of 512 entries, so a count above
  512 * 512 read past it; that product is `PVRDMA_PAGE_DIR_MAX_PAGES`, which
  the header defines for exactly this purpose and which nothing in the tree
  had used. The count is now checked against it, which also caps the mapping
  the caller makes -- previously a guest-chosen count could reserve up to
  16 TiB and drive a walk of four billion iterations. Widening the product
  above narrowed the mismatch case but left the count unbounded, so this
  completes that fix rather than replacing it.

### Limitations

### Known issues

* Several unchecked 32-bit sums of the same class as the bounds-check fixes
  above remain in the TCP backend and are not addressed here. The receive and
  read-completion paths clamp a per-SGE copy with
  `if (bytes_copied + to_copy > data_len)`, whose `uint32_t` addition a
  guest-supplied SGE length can wrap so that the clamp is skipped and the copy
  runs well past the payload. The DMA memory-region path in `tcp_wr_map_sge()`
  compounds this: `pci_dma_map()` may map less than was asked for, but its
  in/out length is ignored and the work request keeps the full requested
  length. `tcp_wr_map_sge()` also stores an unbounded `num_sge` alongside a
  32-entry array; consumers iterate to `num_sge` without their own bound, which
  is currently safe only because the PVRDMA command layer rejects larger
  values.
* `create_mr()` still registers a memory region whose host mapping failed,
  with `virt = NULL` and the guest's own `start` and `length`, so that the
  loopback backend can allocate its keys from the metadata alone. Both
  backends now reject such a region at the point of use -- the TCP one in
  `tcp_mr_range_ok()`, the loopback one by skipping unmapped regions during
  translation -- but the registration itself is unchanged, and neither
  `cmd->start` nor `cmd->length` is validated before it.
