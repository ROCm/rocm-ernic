# Changelog for rocm-ernic

## UNRELEASED - rocm-ernic 0.0.1

### Added

* `-L` / `--log-level` and the `ERNIC_LOG_LEVEL` environment variable select
  the server log verbosity (`none`, `error`, `warn`, `info`, `debug`).
* `-G` / `--socket-group GROUP` lets the members of a group, given by name
  or number, connect to the server's vfio-user socket, which it makes mode
  `0660` with that group before serving it. The server has to run as root or
  be a member of the group. `ERNIC_SOCKET_GROUP` in `rocm-ernic.env` passes
  it to every instance the service starts, and to `ernicctl hot-reload`, so
  a QEMU that doesn't run as root can connect, and still can after a restart.
* The server presents an AMD Pensando ionic NIC (`1dd8:1002`, subsystem
  `1dd8:5400`) so the guest can use the upstream Linux `ionic` and
  `ionic_rdma` drivers. The guest-side patch in `patches/` is built into a DKMS
  package by the `ERNIC_BUILD_KMOD` targets.
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
* `-DERNIC_USE_CODE_COVERAGE=ON` (Clang only) builds with LLVM source-based
  coverage, and the `ernic-coverage` target reports the line coverage of
  `src/` after `ctest`. The Build and Test workflow has a coverage leg that
  fails below a floor, and on `develop` publishes the figure for the README's
  Coverage badge.
* `ernic-test-ionic-harness` (CTest `ionic-harness-*`) runs the whole ionic
  device in-process, with a fake libvfio-user over heap memory, and drives it
  the way the guest drivers do: bring-up, the RDMA admin queue, RC and UD
  transfers, atomics, fast registration, SRQs, CQ events, Ethernet Tx and Rx,
  and the NVMe-oF connect over GSI. It needs no VM or RDMA device, so the data
  path is now tested in every CI build leg, sanitizers included. It raises
  the line coverage of `src/` from about 51% to about 67%, and the coverage
  leg's floor goes from 45% to 60%.
* `-DERNIC_USE_IWYU=ON` runs include-what-you-use on rocm-ernic's own sources
  as they compile, which prints suggestions for `#include` lines to add or
  remove. The suggestions don't fail the build. It needs CMake 3.27 or later
  and is best built with the clang that IWYU is built on. The new IWYU
  workflow runs it on every change to the C sources and lists the
  suggestions, failing only if IWYU itself fails.
* `-DERNIC_USE_CLANG_TIDY=ON` runs clang-tidy on rocm-ernic's own sources as
  they compile, with the checks in the new `.clang-tidy`, and fails the build
  on any finding. It needs CMake 3.27 or later, clang-tidy 19 or later, and
  Clang as the C compiler. `.clang-tidy` enables every group of checks that
  applies to rocm-ernic and, for now, turns off the checks the current code
  doesn't pass. The new clang-tidy workflow runs it with clang-tidy 23 in
  Debug and Release on every change to the C sources.
* rocm-ernic can be built as part of another CMake project with
  `add_subdirectory()`. When it is, it leaves the build type to the parent
  project instead of defaulting it to `Debug`. The CTest `cmake-subproject`
  test builds the server this way.
* The Build and Test workflow's GCC Debug leg without sanitizers also runs
  the tests under valgrind's memcheck, which finds reads of uninitialized
  memory, including uninitialized bytes passed to a system call such as
  `send()`, that the sanitizers don't look for. A test fails on any
  valgrind error, or on a leak that's definitely or possibly lost. The
  tests that start the server, and `tests/test_loopback_ci.sh`, run with
  the server itself under valgrind. The test scripts run it under the
  command in the new `ERNIC_TEST_SERVER_WRAPPER` environment variable, if
  that's set, and `test_server_lifecycle` runs under valgrind with
  `--trace-children=yes`, which follows it into each server it starts.
  The tests that only run a script or CMake are left out.
  `tcp-thread-stop-unit` skips its out-of-descriptors row under valgrind,
  which it detects with `<valgrind/valgrind.h>` when that's installed.

### Changed

* The emulated device now identifies as `1dd8:1002` with subsystem
  `1dd8:5400`, rather than `1022:8001`. A guest image built for an older
  release needs its udev rules updated to match and should refresh `pci.ids`
  with `update-pciids` for the current `lspci` name.
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
* The tests are only built when `BUILD_TESTING` is on, which it is by
  default. `-DBUILD_TESTING=OFF` builds just the server.
* The CMake cache variables that hold the git SHA built into the server are
  now `ERNIC_GIT_SHA_FULL` and `ERNIC_GIT_SHA_SHORT`, rather than
  `GIT_SHA_FULL` and `GIT_SHA_SHORT`, so they can't collide with a parent
  project's.
* The server and tests are linked with the OpenSSF hardening options whenever
  the linker supports them: as position-independent executables, with full
  RELRO (`-z relro -z now`), a non-executable stack (`-z noexecstack`), and
  `--as-needed --no-copy-dt-needed-entries`. Ubuntu's GCC already did all of
  this by default, but other toolchains, such as upstream Clang, didn't.
* `ERNIC_USE_SANITIZERS` now builds with every sanitizer except
  ThreadSanitizer that the compiler supports and that can be combined with
  AddressSanitizer: AddressSanitizer (including LeakSanitizer),
  UndefinedBehaviorSanitizer, and the `float-divide-by-zero` check, plus
  `bounds-strict`, `float-cast-overflow`, `pointer-compare`, and
  `pointer-subtract` with GCC, or `implicit-conversion`, `local-bounds`, and
  `nullability` with Clang. Configuring fails if the C compiler isn't GCC or
  Clang, or can't build a program with the sanitizers.
* The unit tests are no longer built with AddressSanitizer and the other
  `ERNIC_USE_SANITIZERS` sanitizers in every build. Like the server, they're
  only built with sanitizers when `ERNIC_USE_SANITIZERS` or
  `ERNIC_USE_THREAD_SANITIZER` is on, so configuring with the tests no longer
  needs the compiler's sanitizer runtimes. Build with those options to run
  the tests under the sanitizers, as CI does.
* Every sanitizer report is now fatal, so `UBSAN_OPTIONS=halt_on_error=1` and
  `TSAN_OPTIONS=halt_on_error=1` are no longer needed. The programs that
  rocm-ernic builds set some sanitizer run-time options themselves, such as
  the one that turns on GCC's `pointer-compare` and `pointer-subtract`
  checks, which the `ASAN_OPTIONS`, `UBSAN_OPTIONS`, and `TSAN_OPTIONS`
  environment variables override.
* The new `ERNIC_SANITIZER_SHARED_RUNTIME` option selects whether sanitizer
  builds link the shared sanitizer runtime, so that they can load libraries
  from another sanitizer build, or the static one. It defaults to what the
  compiler does on its own, `ON` with GCC and `OFF` with Clang. With Clang
  and `ON`, the programs get an RPATH to the runtime's directory.
* The Ansible Collection workflow runs ansible-lint 26.10.1 with `--strict`,
  so an ansible-lint warning now fails CI as well as an error. A rule the
  code can't meet goes in `skip_list` in `ansible/.ansible-lint`, with the
  reason.
* The ShellCheck workflow and the service test's ShellCheck step no longer
  exclude any checks, so findings such as an unquoted variable expansion
  (SC2086), an unused variable (SC2034), or a `cd` without `|| exit` (SC2164)
  now fail CI. Suppress one only on the command it's about, with a comment
  saying why.
* The ShellCheck workflow also runs actionlint 1.7.12 over the workflows,
  which checks the shell in their `run:` blocks with ShellCheck 0.11.0 and
  every default check on. The self-hosted runner labels it needs are in the new
  `.github/actionlint.yaml`.
* The new `tools/check-embedded-shell.py`, run by the ShellCheck workflow,
  checks the shell that lives inside other files with ShellCheck: the
  Ansible collection's `shell:` and `raw:` tasks, the composite actions'
  `run:` steps, the scripts that heredocs feed to a shell (such as the ones
  the system tests run in their guests), and the workflow `run:` blocks
  that GitHub runs under `sh`. It needs PyYAML.
* **Breaking:** the server's vfio-user socket is now mode `0600`, so only
  the server's user and root can connect, rather than `0666`, and
  `rocm-ernic-launcher` and `ernicctl hot-reload` no longer make the instance
  sockets `0666`. Whoever connects to the socket drives the emulated device
  as the guest's QEMU, so any local user could take over an instance, or
  keep the real VM from attaching. The server creates the socket with these
  permissions whatever its umask, rather than changing them after it's
  already listening. QEMU running as the server's user or as root, as with
  `sudo ernicctl vm-launch`, is unaffected. To let a QEMU running as another
  user connect, use the new `--socket-group` (`ERNIC_SOCKET_GROUP` for the
  service). The server also refuses to start if the socket ends up with
  other permissions or an ACL, which a default ACL on its directory can
  cause, since an ACL entry naming another user lets that user connect
  without the mode showing it; remove the default ACL (`setfacl -k`) or use
  another directory.
* A TCP mesh worker now ignores a topology update that doesn't come on its
  own connection to the manager, as it already ignored the GID table. An
  update decides which other workers a worker connects to, and now also which
  of its connections to them it closes, so another worker could otherwise
  have it drop its links.

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
* The PVRDMA device's data path in the `tcp` and `loopback` backends, which
  nothing has used since that device was removed: the ionic data path moves
  the data itself. A TCP mesh node now ignores the data-path messages that
  path sent (sends, receives, completions, and RDMA writes and reads) rather
  than acting on them. Data that came for a queue pair with no receive posted
  used to be kept until the guest destroyed the queue pair, so any mesh peer
  could grow a node's memory without bound. The `loopback` backend's `mode=`
  and `md5` options went with it; `--backend loopback:...` still accepts them
  and ignores them. The unit tests of that code, `tcp-mr-bounds-unit` and
  `loopback-sge-copy-unit`, are gone too. So are the bugs found in it, which
  a guest or mesh peer could reach and which had been fixed in place: memory
  region bounds checks and scatter-gather length sums that could wrap, a
  region with no host mapping used as if it had one, a work request partly
  mapped or with more entries than it holds, DMA mappings leaked on local
  sends, and a rejected local RDMA write reported to the guest as a success.

### Fixed

* The test scripts, the self-hosted CI scripts in `ci/`, and the developer
  scripts in `scripts/` find bash through `PATH` (`#!/usr/bin/env bash`)
  instead of requiring `/bin/bash`, so `ctest` and the scripts run on systems
  without it, such as NixOS. The installed service and driver-pack scripts
  still use `/bin/bash`.
* Shutting down a `loopback`, `nvmeof` or `s3` server that a guest had
  connected to locked a mutex and wrote past the end of the backend's state
  on the heap: the data path cleared its mesh receive callback through the
  TCP backend whatever the backend was. The TCP backend's ionic entry points
  now do nothing on another backend. Found by `ionic-harness-*`.
* Shutdown leaked the queues and mutex of every CQ and QP the loopback
  backend held, and the deferred-completion queue set up when the device was
  realized.
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
* The `--stats-file` file is created with mode `0644`, less the umask, like the
  `--log-file` file. It was created with `0666`, so under a permissive umask
  any local user could rewrite the counters `ernic-exporter` reports.

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
* A server on the TCP backend no longer hangs on SIGTERM. Its accept thread
  stayed blocked in `accept()` until a peer happened to connect, and a
  manager's health-check thread slept out the whole
  `ERNIC_TCP_HEALTH_INTERVAL` before it noticed the shutdown; shutdown now
  wakes both. A worker that fails to register with its manager also stops its
  accept thread before freeing the state that thread uses, and its 5-second
  registration timeout is now measured on the monotonic clock, so a change to
  the system time no longer shortens or stretches it.
  `tests/test_tcp_thread_stop.c` (CTest `tcp-thread-stop-unit`) covers both
  threads, and `server-lifecycle` checks that a manager exits promptly.
  `tcp-mesh-ci` no longer SIGKILLs its nodes: a node still running 30 seconds
  after SIGTERM, or one that exits with a non-zero status, fails the test.
* A TCP mesh node no longer frees its backend at shutdown while receive
  threads are still using it. A manager freed its table of mesh nodes while
  those threads could still read it, which ThreadSanitizer reported, and never
  stopped or freed the connection of a peer that connected but didn't
  register, such as one it refused for running an older protocol version,
  which LeakSanitizer reported. The backend now stops every one of its threads
  before it frees any of its state. A thread waiting for the rest of a message
  now also gives up when it is stopped, so a peer that sends a message header
  and withholds the payload can no longer hold up shutdown, whether it does so
  to a receive thread or to a worker's accept thread during the handshake. Nor
  can a peer that stops reading. A thread sending to it -- a receive thread
  replying, a broadcast to every node, the health check's heartbeat -- waits
  for room in the socket while holding locks that shutdown needs, and other
  threads can wait on those locks in turn. Once shutdown begins, such a send
  now gives up instead of waiting, and stopping a receive thread also shuts its
  socket down. Sends now use `MSG_NOSIGNAL`, so a failed send to a peer that
  has gone returns an error instead of raising a SIGPIPE that could kill the
  server. `tcp-thread-stop-unit` covers all three, and runs a manager's whole
  shutdown with a worker that has stopped reading.
* A TCP mesh node that can't start one of its threads, because the process is
  out of threads or memory, no longer tries to join that thread at shutdown,
  which could crash the server. A connection whose receive thread can't be
  started is closed instead of kept, and a node whose accept or health-check
  thread can't be started fails to initialize. `tcp-thread-stop-unit` covers
  it.
* A TCP mesh manager whose reconnect to an unresponsive node fails no longer
  stops that node's old connection a second time on the next attempt. It
  joined the old receive thread again, which is undefined behavior and could
  crash the manager or hang its health check. The old connection's socket is
  also now shut down before its thread is joined, so a send the thread is
  blocked in can't stall the reconnect, and closed under the connection's
  lock, so a thread sending on that connection can't send on the descriptor
  after it has been reused for another connection. `tcp-thread-stop-unit`
  covers the failed reconnect.
* A TCP mesh worker no longer crashes on a malformed handshake from a host
  that connects to its listen port. A handshake with no payload crashed it
  with a NULL pointer dereference, and one too short to hold a node id and
  protocol version was read past its end; both are now refused. So is a
  message of another type followed by a second connection that hangs up or
  sends nothing, which freed the first message's payload a second time. A
  worker also no longer takes up to 5 seconds to shut down while a peer that
  has connected sends nothing. `tcp-thread-stop-unit` covers each case.
* A TCP mesh manager no longer registers a connection a second time when a
  peer sends a second registration on it. The connection was given a second
  node id and kept under both, so shutdown freed it twice, and a reconnect
  that replaced one of the two could free it while the other was still in
  use. Any host that could reach the manager's port could crash it this way.
  A registration on a connection that has already registered is now ignored.
  `tcp-thread-stop-unit` covers it.
* A TCP mesh manager no longer deadlocks when a node it has declared dead
  sends a heartbeat while the manager reconnects to it. The health check held
  the mesh table's lock throughout, and the reconnect waits for the old
  connection's receive thread, which needed that lock to record the
  heartbeat; the manager then stopped registering nodes, and hung on SIGTERM.
  The health check now holds the lock only to decide what to do, so
  registrations and heartbeats no longer wait for a reconnect, or for a
  heartbeat sent to a node that has stopped reading, and a node registering
  while the health check reports a dead node can no longer cut its pass
  short. A node that answers while the manager is reconnecting to it keeps
  its connection. The reconnects in each health-check pass now give up on
  nodes that don't answer after one health-check interval
  (`ERNIC_TCP_HEALTH_INTERVAL`, 5 seconds by default) between them, rather
  than when the kernel gives up on each, about two minutes later. No
  heartbeats go out while they wait, so several unreachable nodes could
  otherwise get the nodes that were still answering, or had just
  registered, declared dead. Nodes that a pass runs out of time for go first
  in the next one, in the order they were put off, so nodes that never answer
  can't keep the others from being reconnected. SIGTERM no longer waits for a reconnect either.
  `tcp-thread-stop-unit` covers each case.
* A TCP mesh worker's RDMA traffic no longer stalls while it connects to the
  peers the manager tells it about. It held the connection table's lock,
  which the data path takes to find a peer's connection, across each
  connect, so a peer that didn't answer stalled the worker until the kernel
  gave up on the connect, about two minutes later, and SIGTERM waited as
  long. The connects also ran on the thread that answers the manager's
  heartbeats, so the manager could declare the worker dead meanwhile. A
  worker now connects to its peers on a thread of its own, without the lock,
  and SIGTERM no longer waits for a connect. Each connect gives up after one
  health-check interval (`ERNIC_TCP_HEALTH_INTERVAL`, 5 seconds by default),
  and every peer is tried in turn, so one that doesn't answer can't keep the
  worker from connecting to the others. A peer the worker couldn't connect
  to was left unconnected until the mesh changed; it is now tried again
  every interval or so. A worker also no longer tries to connect to peers
  the manager reports dead, including one reported dead while the worker is
  connecting to it or to another peer. `tcp-thread-stop-unit` covers each
  case.
* A TCP mesh node no longer drops a connection when a message header arrives
  in parts. A header that had partly arrived was given up on and the part
  read was lost, so the next read started in the middle of the header, failed
  the protocol magic check, and closed the connection. A header can straddle
  a TCP segment boundary anywhere in a busy connection, so this could happen
  under load on a real network. A node now waits for the rest of the header,
  as it already did for the rest of a payload. A worker's listen port gives a
  connecting peer 5 seconds to send all of its handshake, including the rest
  of one it has started, so a peer that sends part of one and stops holds up
  the peers behind it no longer than one that sends nothing.
  `tcp-thread-stop-unit` covers messages split at several points.
* A TCP mesh manager now closes the connections it is done with. A peer that
  connected and hung up without registering -- a port scan, a load
  balancer's health check, a node refused for running an older protocol
  version -- left its socket open in `CLOSE_WAIT`, and a thread behind it,
  until the manager shut down, and a peer that connected and sent nothing
  kept them indefinitely. A registered node that went away left its socket
  open until the manager reconnected to it, which might be never. Enough of
  these ran the manager out of file descriptors. The manager now closes a
  connection whose peer has hung up or been refused when the next peer
  connects, or within a health-check interval (`ERNIC_TCP_HEALTH_INTERVAL`,
  5 seconds by default), and closes one that hasn't registered within 5
  seconds of connecting. At most 64 connections wait to register at once;
  any more are closed as they arrive. A peer that sends anything but a
  registration before it has registered is now disconnected: the manager
  used to act on such messages, including relaying traffic to other nodes,
  and a relay to a node that had stopped reading could then hold up
  closing the connection. A registered node that hangs up is
  reported dead to the rest of the mesh within a health-check interval,
  rather than after three. A node that runs out of file descriptors also no
  longer retries accepting a connection in a tight loop that logged an error
  each time, which could fill the disk; it now waits 100 ms between
  attempts. `tcp-thread-stop-unit` covers each case.
* A TCP mesh node no longer trusts the sender a message names in its header,
  which the sender writes. A manager credited a heartbeat to the node the
  heartbeat named, so any registered node could keep another, dead, node
  alive, or bring it back; it now credits the node whose connection the
  heartbeat came on. Every node sent its answer to an RDMA write or read, or
  to a send, to the node the request named, so a peer could have the
  completion, or the memory it asked to read, delivered to another node; the
  answer now goes back on the connection the request came on.
  `tcp-thread-stop-unit` covers each case.
* A TCP mesh node that the manager has declared dead and that answers a
  heartbeat again is now reported alive to the rest of the mesh at once. The
  other nodes used to keep treating it as dead, and not connect to it, until
  a node joined, died, or was reconnected. Topology updates sent at about the
  same time can also no longer reach the nodes out of order, which could
  leave them with an older picture of the mesh than the manager's.
  `tcp-thread-stop-unit` covers the revival.
* A TCP mesh manager that reconnects to a node that has stopped answering no
  longer frees the node's old connection while another thread is still
  using it, sending on it or relaying through it. Connections are now
  reference counted, and the last thread to finish with one frees it. The
  connection of a node that hangs up, or that the manager reconnects to, is
  also taken out of the manager's connection table, instead of staying there
  closed until shutdown.
* Data that reaches a TCP mesh node before the guest has posted a receive for
  it is now completed on the connection it came on, once the receive is
  posted. The completion used to go to whichever connection the node named
  as the data's sender had by then, so a peer could have it delivered to
  another node. If that connection has closed meanwhile, no completion is
  sent. `tcp-thread-stop-unit` covers both fixes.
* A TCP mesh worker now takes its node id only from the manager, and only
  once. It used to accept the answer to its registration from any
  connection, at any time, so another worker could change its node id, and
  the GIDs it claims, by sending it one. A worker also no longer accepts
  connections from its peers, or connects to them, before it has its id. It
  used to introduce itself as node 0, the manager's id, to a peer that
  connected while it was registering, and to its peers when a topology update
  arrived before the manager's answer. A manager that refuses the
  registration, or assigns 0 or `UINT32_MAX`, now makes the worker fail at
  once rather than after the 5-second registration timeout.
  `tcp-thread-stop-unit` covers each case.
* A TCP mesh worker now reconnects to another worker when the connection
  between them drops, or when the manager reports the other worker dead and
  then alive again. It used to keep the dead connection, with its socket
  open in `CLOSE_WAIT`, until it shut down, and the two workers never
  connected again, because each refused or skipped a new connection as a
  duplicate of the dead one. Traffic between them was sent into the dead
  connection meanwhile, and failed; it now goes through the manager until
  they reconnect. A worker now closes a dead connection within a
  health-check interval (`ERNIC_TCP_HEALTH_INTERVAL`, 5 seconds by default),
  and connects again if it has the lower id; one with the higher id replaces
  the dead connection when the other connects again. A worker also closes its
  connection to a worker the manager reports dead, so the connection to one
  that went away without closing it, in a crash or a network partition, no
  longer looks connected until a send fails. A second connection claiming to
  be a worker that is still connected is still refused.
  `tcp-thread-stop-unit` covers each case.
* The server no longer exits when a client disconnects or sends something
  other than a version message during vfio-user negotiation. A port probe or a
  client that crashed while connecting used to take the server down; it now
  logs `client dropped during negotiation` and waits for the next client.
  Other attach errors are still fatal.
* The server frees the emulated ionic device at shutdown, and no longer leaks
  the parsed path of an `nvmeof` backend's `file=` namespace. LeakSanitizer
  and valgrind reported both. `tests/test_server_lifecycle.c` (CTest
  `server-lifecycle`) starts the real server and checks that misbehaving
  clients and SIGTERM/SIGINT end in a clean shutdown with no sanitizer report.
* The Nix `analysis-sanitizers`, `analysis-tsan`, and `analysis-valgrind`
  targets no longer report a clean run when the server never got through
  the exercise, so its leak checks never ran. The first line of
  `summary.txt` is now `exercise: clean` or
  `exercise: incomplete (<reason>)`, and an incomplete exercise fails the
  build: a server that never started listening, a client that failed or
  timed out, a server that never handled the client's connection, or one
  that didn't shut down cleanly. Sanitizer and valgrind
  findings still don't fail it, including a sanitizer finding that stopped
  the server before the exercise finished.
* The `malloc-no-null-check` and `goto-usage` rules of the Nix
  `analysis-semgrep` target never parsed, so semgrep skipped them on every
  run. Both run now, so every `goto` is reported, at `INFO`.
  `malloc-no-null-check` follows the pointer's data flow, so a NULL check
  covers only the allocation it tests, and recognizes checks that return,
  `goto`, `exit()` or `abort()`.
  `analysis-semgrep` tests its rules against the fixtures in
  `nix/analysis/semgrep-tests/` and fails if one doesn't pass, if a rule
  doesn't parse, or if the scan fails, and reports source that semgrep
  could only partly parse. The Nix Check workflow builds the new
  `analysis-runners` check, which tests these targets' scripts against a
  fake server, client and scan.

### Limitations

### Known issues

* While the TCP backend is running, a mesh node that stops reading can stall
  the node sending to it. A send waits for room in the socket for as long as
  it takes, and a broadcast to every node does that while holding the
  connection table's lock, which the data path needs too. Only shutdown makes
  such a send give up.
* The TCP backend still looks up mesh nodes' hostnames with a lookup that
  can't be interrupted or given a deadline. A worker advertises its
  `gethostname()`, which its peers and the manager's health check look up
  before each connect, so a slow DNS server can make SIGTERM wait for the
  resolver to give up, which its configured timeouts and retries bound. A
  hostname that resolves from `/etc/hosts`, or a numeric address, doesn't
  wait.
* A TCP mesh worker whose connection to the manager drops doesn't connect to
  it again. The manager connects to the worker instead, but the worker takes
  topology updates and GID tables only on the connection it opened itself, so
  it ignores the manager's from then on, and doesn't learn of workers that
  join, die, or come back, or of their GIDs, until it restarts.
