# Changelog for rocm-ernic

## UNRELEASED - rocm-ernic 0.0.1

### Added

* `-L` / `--log-level` and the `ERNIC_LOG_LEVEL` environment variable select
  the server log verbosity (`none`, `error`, `warn`, `info`, `debug`).
* The server presents an AMD Pensando ionic NIC (`1dd8:100a`) so the guest can
  use the upstream Linux `ionic` and `ionic_rdma` drivers. The guest-side
  patches live in `patches/` and are built into a DKMS package by the
  `ERNIC_BUILD_KMOD` targets.
* `-T` / `--tap IFNAME` attaches the emulated Ethernet interface to a host TAP,
  giving the guest a routable Ethernet segment with working ARP, ICMP, and
  TCP/IP.
* `tests/test_ionic_ci.sh` (CTest `ionic-ci`) covers the ionic device
  identity, BAR geometry, shutdown, and `--tap` handling without needing a VM.
* The ionic data path now executes the fast-registration work requests
  `IONIC_V1_OP_REG_MR` and `IONIC_V1_OP_LOCAL_INV`. They are local
  operations, so they complete by SQ index whether or not the work request
  asked to be signalled. A stock `nvme-rdma` initiator posts a REG_MR ahead
  of every command, so without these the keys in its keyed SGLs named
  nothing and the QP went to error on the first I/O.
* `--backend nvmeof[:size=...,bs=...,file=...,nqn=...,ip=...,port=...]` runs an
  NVMe over Fabrics target inside the server, reachable from the guest with a
  stock `nvme connect -t rdma`. One VM and one server instance are then a
  complete fabric: the backend answers the IB CM exchange on GSI and executes
  NVMe command capsules, moving data with RDMA READ/WRITE against the guest's
  memory keys. The namespace is anonymous memory by default, or a file with
  `file=PATH`. Documented in `docs/nvmeof.rst`.
* `tests/test_nvmeof_target.c` (CTest `nvmeof-target-unit`),
  `tests/test_nvmeof_cm.c` (`nvmeof-cm-unit`) and `tests/test_nvmeof_ci.sh`
  (`nvmeof-ci`) cover the controller without needing a VM;
  `ansible/playbooks/nvmeof-tests.yml` covers the guest side -- discover,
  connect, an `O_DIRECT` digest round trip, fio, disconnect -- and is run by
  the hosted `system-test-nvmeof` job, by the `vm-nvmeof` self-hosted job via
  `ci/jobs/vm-nvmeof.sh`, and by hand with `--tags nvmeof`.
* `--backend s3[:bucket=...,size=...,objects=...,ip=...,port=...,maxpart=...]`
  runs an S3-over-RDMA object store inside the server. Object payloads move
  by RDMA against the client's registered buffer, described by the
  `x-amz-rdma-token` header cuObject v1.2.0 introduced and hipObject sends;
  the reply carries `x-amz-rdma-reply` and `x-amz-rdma-bytes`. The control
  plane is terminated in band on the emulated wire by `src/s3_tcp.c` — ARP,
  ICMP and a minimal TCP — so the endpoint is simply a host on the guest's
  segment, ARPable at a MAC derived from its address, and needs no `--tap`.
  S3 semantics follow versitygw; nothing is vendored from it. Requests with
  no token fall back to carrying the payload in the HTTP body, so `curl` is a
  usable client. Documented in `docs/s3.rst`.
* `tests/test_s3_token.c` (CTest `s3-token-unit`), `tests/test_s3_http.c`
  (`s3-http-unit`), `tests/test_s3_target.c` (`s3-target-unit`),
  `tests/test_s3_tcp.c` (`s3-tcp-unit`) and `tests/test_s3_ci.sh` (`s3-ci`)
  cover the store without needing a VM; `tests/s3_rdma_client.c` is a
  self-contained libibverbs client built and run inside the guest by
  `ansible/playbooks/s3-tests.yml`, which the hosted `system-test-s3` job runs
  end to end, the `vm-s3` self-hosted job runs via `ci/jobs/vm-s3.sh`, and
  which can be run by hand with `--tags s3`. The hosted lane publishes an
  *S3-over-RDMA GET bandwidth* trend and the `S3 1M GET` shield.
* `ERNIC_BACKEND` in `service/rocm-ernic.env` overrides the backend the
  launcher picks per instance, which is what selects `nvmeof` for that lane.
* CI job `ionic-patches` verifies that every patch in `patches/` still applies
  to the pinned `IONIC_KERNEL_REF`.

### Changed

* The emulated device now identifies as `1dd8:100a` — the Pensando vendor ID
  the upstream driver already claims, with a device ID outside the range real
  hardware uses — rather than `1022:8001`. A guest image built for an older
  release needs its udev rules and `pci.ids` entry updated to match.
* Guest Ethernet leaves through a host TAP, so each instance needs its own TAP
  enslaved to a shared bridge for guest-to-guest IP. The `ernic_host_setup`
  role creates them from `ERNIC_TAP_PREFIX` / `ERNIC_TAP_BRIDGE`, and
  `ci/runner/install-runner.sh` does the same one-time root step for the
  self-hosted CI node.
* rocm-xio is built against the ionic provider (`-DGDA_IONIC=ON
  -DRDMA_CORE_BUILD=OFF`) with the `rdma-core/ionic-gda` patches applied to
  the guest's rdma-core, rather than the `rocm_ernic` GDA backend.
* The server now defaults to the `warn` log level, so the per-operation `INFO:`
  lines (BAR writes, QP operations, ARP/ICMP packets, TCP mesh events) are
  suppressed in steady state. Use `--log-level info` to restore the previous
  output. `-v` / `--verbose` is now shorthand for `--log-level debug`.
* `ernic-exporter` now reports its cumulative series with `TYPE counter`
  instead of `TYPE gauge`, via a custom collector. Metric names are unchanged
  and `rate()` / `increase()` queries keep working, so the shipped dashboard
  needs no edit for this.
* **Breaking:** the two cluster-level gauges lost their misleading `_total`
  suffix — `ernic_instances_total` is now `ernic_instances` and
  `ernic_vms_total` is now `ernic_vms`. They count what exists right now, not
  a running total. The shipped dashboard is updated; hand-written dashboards,
  alerts and recording rules referring to the old names must be changed.

### Removed

* The PVRDMA-derived device personality and everything that only served it:
  the `--legacy` / `--pvrdma` flags and the `-I` / `--ionic` no-op, the
  out-of-tree `rocm_ernic_eth` / `rocm_ernic_rdma` guest driver in `driver/`,
  the `rocm_ernic` rdma-core provider and its direct-verbs headers, the
  `ERNIC_DEVICE_MODE` and `ernic_device_mode` settings, `CI_ERNIC_MODE`, and
  the per-run device-mode annotation in `ci/report/publish-perf.py`. The
  emulated device is now always ionic and there is no flag to select it.
* The milestone benchmark tables in `docs/performance.rst`. They measured the
  removed device; the live ionic numbers are in `docs/perf-trends.rst`.

### Fixed

* The driver pack recorded a hardcoded `v7.2.4` as the ionic baseline
  regardless of `IONIC_KERNEL_REF`, so bumping the cmake pin left guests
  fetching the old sources while the patches shipped beside them came from the
  new ones. The pin is now written to `share/rocm-ernic/ionic-kernel-ref` at
  install time and copied into the tarball from there. The undocumented
  `ERNIC_IONIC_KERNEL_REF` environment hook — referenced once, assigned
  nowhere — is removed rather than kept as an override, and neither
  `rocm-ernic-driver-pack` nor `vm-driver-install.sh` falls back to a literal
  version any more: a pack without the ref file fails instead of building from
  a baseline its patches do not match.
* Per-QP opcode lines in the `*.stats` file are emitted with a wide enough
  field to keep the `" : "` separator that `ernic-exporter` splits on.
  `MASKED_ATOMIC_CMP_SWP` and `ATOMIC_FETCH_AND_ADD` filled the old 20-column
  field exactly, so their lines came out as `NAME: 4` and were dropped by the
  parser. The exporter also no longer attributes a device-level key that
  appears after the per-QP block to the last QP it saw.

* MSI-X assertions raised while a vector is masked are latched and replayed
  when the driver unmasks, instead of being dropped. Every vector comes out of
  reset masked, so an interrupt raised in the window before the guest arms its
  handler was discarded with no pending state to recover it, and the queue
  waited forever for an interrupt that would never be sent again. The emulator
  now records the assertion per vector and delivers it from both unmask paths:
  a direct write of 0 to the interrupt mask register and an `INTR_CRED_UNMASK`
  credit return. `tests/test_ionic_intr_pending.c` (CTest
  `ionic-intr-pending-unit`) covers both paths, mask-on-assert re-latching, and
  the collapse of repeated assertions into one delivery.
* A local loopback RDMA write whose bounds check fails is now reported to the
  guest as a failure. The check in `tcp_post_send()` correctly refused the
  copy, but execution then fell through to an unconditional statistics update
  and an `IBV_WC_SUCCESS` completion, so the guest was told a write that had
  copied nothing had succeeded and went on reading whatever the target region
  held before. The same fault on the remote path has always failed the work
  request; the loopback shortcut now reaches the same verdict without the
  wire, completing with `IBV_WC_REM_ACCESS_ERR` and zero bytes, and the byte
  counters are updated only after a copy that actually happened. Reachable
  from the guest by posting a write to a loopback queue pair with a bad key,
  a region with no host mapping, or an out-of-range address.
* `tcp_wr_map_sge()` now refuses a work request whole when any of its
  scatter-gather entries cannot be mapped, instead of zeroing that entry's
  length and continuing. Every copy loop downstream walks the entries in order
  and advances its destination cursor only for entries it actually copies, so
  a skipped entry did not leave a hole -- it slid all the later data down by
  that entry's length, writing the right number of bytes to the wrong offsets
  and completing successfully. Both callers now fail the request with
  `IBV_WC_LOC_PROT_ERR`.
* `tcp_wr_map_sge()` now bounds `num_sge` against the 32-entry array it fills.
  The count was stored in `wr->num_sge` and travels to the peer inside the
  work request, where receive-side loops iterate to it without re-clamping;
  this was previously safe only because the PVRDMA command layer rejected
  larger values first.
* The local loopback path in `tcp_post_send()` now releases the DMA mappings
  taken by `tcp_wr_map_sge()` before freeing the work request, and pops the
  send queue under `priv->lock`. The mappings were leaked on every loopback
  send, and the pop without the lock raced the two receive-thread sites that
  pop the same queue.
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
  It does not cover every unchecked sum in the data path -- see "Known issues"
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
  `start` and `length`. The bounds check always passed by construction and
  the offset was then added to `NULL`, turning an out-of-region write into an
  absolute write at a guest-chosen address. The region's backing is now part
  of the bounds check, alongside its extent.
* The remote send path in `tcp_post_send()` now sums scatter-gather lengths in
  64 bits and bounds the result before it reaches the wire. The previous
  32-bit sum sized two heap buffers whose fill loops wrote the full unwrapped
  per-entry lengths, so a wrapped total undersized the allocation rather than
  merely reporting it incorrectly. The receive path enforces its ceiling per message
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
  16 * 2^40 bytes and drive a walk of four billion iterations. Widening the product
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
  length.
* `create_mr()` still registers a memory region whose host mapping failed,
  with `virt = NULL` and the guest's own `start` and `length`, so that the
  loopback backend can allocate its keys from the metadata alone. Both
  backends now reject such a region at the point of use -- the TCP one in
  `tcp_mr_range_ok()`, the loopback one by skipping regions without a mapping during
  translation -- but the registration itself is unchanged, and neither
  `cmd->start` nor `cmd->length` is validated before it.
