# Changelog for rocm-ernic

## UNRELEASED - rocm-ernic 0.0.1

### Fixed

* **Breaking:** **The ionic DKMS package is built only for the kernel it
  was asked for.** `setup-ionic-dkms.sh` registered it with
  `AUTOINSTALL="yes"`, so DKMS built it for every kernel with headers
  installed. These sources track the kernel they came from -- the `v7.2.4`
  ionic includes `<rdma/iter.h>`, which no 7.0 kernel has -- so on a guest
  carrying both its running kernel and the distribution's own, one of those
  builds could never succeed, and it failed the whole run. It also hid the
  reason: DKMS gives a module one build directory shared by every kernel,
  so the losing build left its `make.log` there under the other's name, and
  the error reported was a missing header on a kernel nobody had asked
  about. **After a kernel upgrade, run the script again** (or re-run
  `ernic_guest_setup`, or `ernic_srq_dkms` for the SRQ series); DKMS will
  no longer rebuild these modules by itself.
* **A failed ionic DKMS build reports the right `make.log`.** It now
  prefers DKMS's per-kernel copy, falls back to the shared build directory,
  and says when the log it found was written for a different kernel than
  the one that failed.
* **`tcp-thread-stop-unit` no longer trips its watchdog under valgrind.**
  The per-row watchdog is a hang detector, but its ten seconds were a
  native-speed budget, and the slowest legitimate row -- 64 peers connected
  one at a time, each with a receive thread, then a 65th -- runs every one
  of those threads on one core under valgrind. It took well under a second
  natively and ran out of time on a loaded four-CPU CI runner with 63 of
  the 64 peers connected, which failed the valgrind leg intermittently.
  The budget is six times larger under valgrind, which is still far
  below what a genuinely stuck row would take.
* **The thread-stop test's watchdog says what it was waiting for.** When a
  row runs over its timeout the watchdog kills the process, which leaves
  every thread running, and valgrind then reports each one's thread-local
  storage as possibly lost -- so a timeout was reported as a memory leak
  whose stacks pointed at whatever had created the threads, and the leak
  was the only part anyone saw. It now prints the threads created, joined
  and exited alongside the row name, which is what identified the row above
  as merely unfinished rather than stuck.

* **An extern that already wrote the destination no longer lets the
  ordinary transfer land on top of it.** An action may call the extern
  more than once -- `cvt()` and then `map()`, say -- and the second call
  declines when the work request is not that operation's shape. The
  dispatch-as-an-ordinary-transfer path then ran even though the first
  call had written the result, so the guest got its raw payload where it
  asked for a converted one, with a successful completion. The
  fall-through is now taken only when nothing ran;
  `tests/data/p4/fp8_cvt_then_map.p4` is the program that makes it
  happen, and the device refuses it.
* **A declining extern no longer overrides a drop the program already
  made.** When the extern declines a request that is not its shape, the
  pipeline stops at the call, and the drop verdict was lost with it: a
  program that called `mark_to_drop()` first -- earlier in the action, or
  in an access-list table ahead of the compute table -- had the RDMA WRITE
  it denied dispatched anyway. The drop now stands, and the operation
  completes without transferring, as any dropped operation does.
  `tests/data/p4/fp8_drop_then_map.p4` covers it.
* **An out-of-range `enum p4_fmt` is refused rather than indexing the
  format table.** `p4_fmt_valid()` took a `uint8_t`, so a wider bad value
  was narrowed before it was checked -- `(enum p4_fmt)256` validated as
  format 0 and then read `fmt_table[256]`. It takes the enum now. Not
  reachable from a guest, whose formats arrive as bytes, but the check
  was in the wrong order.

* **A P4 program that selects an fp8 reduction no longer breaks ordinary
  RDMA WRITE.** A table keyed on the opcode -- which is what
  `tests/data/p4/fp8_policy.p4` does, and the only key that can select a
  reduction -- matches every RDMA WRITE, not just the two-equal-operand
  compute shape. The extern used to fail anything else, and the device
  turned that into a local QP operation error, so a guest that installed a
  reduce entry lost ordinary RDMA WRITE on every QP of the device: a
  one-SGE write, a two-SGE write with unequal lengths and a three-SGE
  write all killed their QP. A request that is not the compute shape is
  now declined by the extern and dispatched as an ordinary transfer, as it
  would be with no program loaded. A request that *is* the compute shape
  but is otherwise bad still fails.
* **Loading a P4 program no longer costs the guest its Ethernet.** A
  program that runs without assigning `standard_metadata.egress_spec` has
  not chosen a port; the frame is now forwarded to the other port, which is
  what the device does with no program at all. The field's initial zero
  used to be read as `P4_PORT_HOST`, so every frame a guest transmitted was
  injected straight back into its own receive ring -- a silent loopback,
  counted as forwarded rather than dropped, with no error anywhere. Every
  program written for the RDMA path does this, `fp8_policy.p4` included, so
  loading the worked example took the guest's network away and with it the
  ARP and neighbour discovery that RoCE address resolution needs.
* **The `rdma_*` counters in the stats file's LIF block now carry real
  values.** They were named in `--stats-file`, and in the LIF statistics
  block DMAed to the guest, but nothing ever incremented them: they read
  zero for ever while a guest reading `hw_counters` saw the real figures.
  The datapath's RDMA error counters are now copied into the LIF block
  before either consumer reads it. A device reset zeroes them where the
  datapath keeps them, so the guest's `hw_counters`, device and per-QP,
  now restart from zero on a device reset too, as the rest of the LIF
  block always has.

### Added

* **The P4 fp8 extern converts between formats as well as reducing.** A
  new method, `cvt(src_fmt, dst_fmt)`, reads one region and writes the
  same number of elements in another format -- **in either direction**,
  so the destination may be longer than the source, shorter, or the same
  length. The table entry names both formats, so one program narrows or
  widens depending only on what the control plane put in it.

  Six formats, as the numbers a table entry carries: `0` fp32, `1` fp16,
  `2` bf16, `3` fp8 E4M3, `4` fp8 E5M2, `5` **fp4 E2M1** -- the OCP
  microscaling 4-bit format the AMD data paths use, packed two elements
  to a byte, low nibble first. An odd count of a 4-bit format is half a
  byte at one end and is refused rather than rounded.

  A work request asks for a conversion by its gather list, as the
  reduction does: two equal entries is a reduction, **one entry is a
  conversion**, and anything else is an ordinary transfer. It is the only
  operation whose destination is not the same length as its source, so
  each region is checked against its own length and nothing past the
  result is written. `tests/data/p4/fp8_convert.p4` is the worked
  example, with `fp8_convert.json` compiled from it.

  Conversion follows the same conventions as the rest of the fp8 code:
  round to nearest with ties to even, and a magnitude too large for the
  destination becomes an infinity if that format has one and saturates to
  its largest finite magnitude if it does not -- an infinity narrowing
  into E4M3 or FP4 saturates for the same reason an overflow does. A NaN
  survives wherever there is an encoding for one; FP4 E2M1 has neither
  infinity nor NaN, so a NaN saturates there too, sign preserved.
* **The P4 lane records a conversion trend.** It measures fp16-to-fp8
  throughput at 4 KiB and 64 KiB and publishes a `p4cvt` series, so
  `perf-trends` gains a P4 conversion chart. The number is the whole
  guest-to-result path, not the arithmetic alone. 1 MiB is absent by
  construction: a conversion is capped at 256 Ki elements, which 1 MiB of
  a 16-bit source is twice over.
* **The Python driver reaches the P4 control surface.** `tests/py` can
  now IDENT the surface, push a program down in chunks, install table
  entries and read the engine's counters, so the conversion is covered
  over a real vfio-user socket with no VM at all -- narrowing, widening,
  and a two-entry write staying an ordinary transfer.


* Nightly DEB packages: every night that `develop` changes, the `nightly`
  prerelease is replaced with packages of it for Ubuntu 24.04 and 26.04
  (`.github/workflows/nightly.yml`, modelled on hipObject's). `rocm-ernic`
  has the server, the systemd service, `ernicctl`, and the guest driver
  pack, installed under `/usr`, with `/etc/rocm-ernic/rocm-ernic.env` as a
  configuration file that upgrades keep; `rocm-ernic-exporter` has the
  Prometheus exporter. They're unsupported development builds. The server in
  them has its own copy of libvfio-user, so nothing else needs installing;
  see INSTALL.md.
* `cpack -G DEB` in a build directory makes those packages (see Packages in
  docs/building.rst). `-DERNIC_BUNDLE_VFIO_USER=ON` installs a private copy
  of the libvfio-user the server was linked with, in
  `<libdir>/rocm-ernic`, which only the installed server loads, through a
  `DT_RPATH` that `LD_LIBRARY_PATH` can't override: libvfio-user keeps the
  soname `libvfio-user.so.0` through incompatible changes, so no other copy
  can be trusted to match. It needs `ERNIC_VFIO_USER_LICENSE` set to
  libvfio-user's `LICENSE`, which is installed with it, and fails to
  configure unless the library is linked with full RELRO, like the server.
  `ERNIC_PACKAGE_RELEASE` sets the release part of the packages' version
  (default `local`); it can't be a value CMake takes as false, such as `0`,
  which CPack would leave out of the version.
* An install now includes rocm-ernic's licenses, in
  `<prefix>/share/doc/rocm-ernic`.
* The `ernic-package-deb` CTest test builds the DEB packages and checks
  them: exactly the expected files in each package, their modes, the
  dependencies (including one for every library the server and its private
  libvfio-user need), the configuration file, which every systemd unit has
  to read, no `/usr/local` paths in a package installed elsewhere, and the
  server's `DT_RPATH`. It's registered in a top-level build when
  `dpkg-dev`, `file`, and `readelf` are installed, and runs in the Build and
  Test workflow, which now configures with `-DERNIC_INSTALL_SERVICE=ON` so
  the service and exporter packages are checked too, and now also runs for
  changes to `prometheus/` and `ansible/files/`, whose files are packaged.
  A change to the install rules has to update the test's file lists
  (`tests/package/run-package-test.cmake`).
* The nightly workflow installs the packages in a clean Ubuntu container,
  without their recommendations, and checks that the server runs with its
  private libvfio-user, that `ernicctl` runs, and that the exporter, set up
  in a virtual environment as docs/monitoring.rst says, serves metrics with
  the command its systemd unit runs. The packages are published only if
  that passes. Pull requests that change what's installed, in `service/`,
  `prometheus/`, or `ansible/files/`, run the workflow too.

* The mesh backend can claim and withdraw a node's GIDs while it is running,
  rather than only announcing the set `ERNIC_TCP_GUEST_GIDS` named at
  startup: the claim is recorded locally and re-advertised to the mesh, and
  peers resolve the address from then on. The mesh protocol is unchanged --
  `TCP_MSG_NODE_GIDS` already carried "this node's current set" and its
  handler replaces rather than merges -- so a node doing this interoperates
  with one that does not.
  **This is an internal entry point, not yet a guest-visible feature.**
  Nothing dispatches a guest's GID-table change to it, so configuring an
  address inside a guest after the mesh has formed still does not advertise
  it. `ERNIC_TCP_GUEST_GIDS` remains the only way to declare a guest's
  addresses, and an address not listed there is still unreachable. What this
  adds is the mesh half of the lifecycle, and a test that exercises it; the
  dispatch from a guest GID update is still to come.
* `ernic-test-gsi-mesh` (CTest `gsi-mesh`) runs two whole devices in one
  process, joined over loopback TCP by the real mesh backend, and checks
  that a GID claimed after the mesh formed resolves to the peer's node from
  both sides. It needs no VM, no RDMA device and no root, so it runs in
  every CI build leg. `ERNIC_TEST_MESH_PORT` moves its listening port, and
  is rejected with a failure if it is not a port number. The test never
  skips: needing nothing optional, every way it can fail to start is a
  regression.
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
  `ionic_rdma` drivers. The guest-side patch in `ansible/files/patches/` is
  built into a DKMS package by the `ERNIC_BUILD_KMOD` targets.
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
  libibverbs client built and run inside the guest by
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
  remove. The suggestions don't fail the build. It is best built with the
  clang that IWYU is built on. The new IWYU workflow runs it on every change
  to the C sources and lists the suggestions, failing only if IWYU itself
  fails.
* `-DERNIC_USE_CLANG_TIDY=ON` runs clang-tidy on rocm-ernic's own sources as
  they compile, with the checks in the new `.clang-tidy`, and fails the build
  on any finding. It needs clang-tidy 19 or later, and Clang as the C
  compiler. `.clang-tidy` enables every group of checks that applies to
  rocm-ernic and, for now, turns off the checks the current code doesn't
  pass. The new clang-tidy workflow runs it with clang-tidy 23 in
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
* `tests/py/` drives real server processes over the vfio-user socket from
  Python, the way a VMM does, with no VM, RDMA hardware, root, or package
  outside the standard library. Guest memory is a `memfd` the client maps and
  the server maps too, so the real DMA path runs. Three suites are registered
  with CTest: `ernic-vfu-client` (the vfio-user protocol, and the offsets,
  counts, addresses and interrupt ranges a client must not get away with),
  `ernic-ionic-driver` (device bring-up, queue pairs, memory regions, work
  requests and completions, through a server process rather than the
  in-process harness) and `ernic-mesh-stress` (a three-node mesh under
  traffic between every pair of nodes, peers sending frames no real node
  sends, the protocol version gate in both directions, and nodes killed and
  restarted while the manager's descriptors are counted). They honour
  `ERNIC_TEST_SERVER_WRAPPER`, so the valgrind leg covers them too, and
  `test_mesh_stress.py --size N` varies the mesh. They need python3, which
  the shell suites already needed. Documented in `docs/testing.rst`.
* Nix outputs for running the emulated NIC under a NixOS guest:
  `nixosModules.ernic-guest` (enable with `virtualisation.ernic.enable`)
  gives a `qemu-vm.nix` guest shared memory, a q35 root port, vfio-user-pci
  and the in-tree ionic drivers; `packages.<system>.ernic-run` starts the
  server around a VM launcher, exports `ERNIC_SOCKET`, and stops the server
  when the launcher exits, passing on its exit code. On x86_64 hosts,
  `nix run .#ionic-vm` boots such a guest to a root shell
  (`--ssh-port N` forwards ssh), and `nix run .#run-ionic-guest-test` checks
  that the guest's RDMA port reaches `PORT_ACTIVE`. The Nix Check workflow
  builds the new `launchers` check, which tests `ernic-run` and the module's
  assertions without a VM. Documented in `nix/README.md`.
* Guests running the in-tree `ionic_rdma` driver get a `hw_counters`
  directory under `/sys/class/infiniband/<dev>/ports/1/`, with device and
  per-QP counters (commands, bytes sent and received, RDMA read and write
  bytes, doorbells, WQEs and CQEs). The LIF identity now advertises
  `stats_type`, and the STATS_HDRS, STATS_VALS, QP_STATS_HDRS and
  QP_STATS_VALS admin commands are implemented; before, the driver created no
  counters at all.
* `ernic-exporter` publishes the ionic LIF statistics block as
  `ernic_lif_counter_total{ernic_id,role,counter}`, one series per named
  field of `struct ionic_lif_stats` under its `ionic_if.h` name (74 of
  them): Ethernet traffic by MAC class, drops, DMA and ring errors, RDMA
  traffic, and the `rdma_req_*` / `rdma_resp_*` error counters. This is the
  block a guest reads for `ip -s link`, and the exporter never published
  any of it before. The counter is in a label rather than in the metric
  name so that a counter the device starts keeping is published without an
  exporter release. The Grafana dashboard gains a **LIF Counters
  (ionic_lif_stats)** row: RDMA packet rate, RDMA error rate, Ethernet drop
  and ring-error rate, and a table of every counter that has moved.
  `docs/monitoring.rst` documents the metric, the separate guest-side
  `hw_counters` view and which exporter metrics already carry it, and how
  both counter sets compare with the silicon the emulator models.
* `ernic-ionic-hw-stats` (CTest) drives the four hardware-statistics admin
  commands over the vfio-user socket the way `ionic_rdma` does: it checks
  that each table describes itself as the driver reads it, that a buffer
  too small for a table gets whole entries only and nothing past its
  length, that values for a QP that does not exist are refused rather than
  zero-filled, and that the `--stats-file` the exporter reads carries the
  same numbers. It drives a SEND, an RDMA WRITE and an RDMA READ of three
  distinct sizes and checks that all five device and all eight per-QP
  counters move by exactly the right amount, on the right QP. The
  structural checks read whatever counter set the device advertises, so a
  counter added to either table is covered without a test change.
* `tools/gen-counter-docs.py` builds the counter lists under
  `docs/generated/` from the C sources that define them --
  `src/ionic_emu_stats.c` for the LIF block, `dev_stats` and `qp_stats` in
  `src/ionic_adminq_priv.h` for the two `hw_counters` tables -- and
  `docs/monitoring.rst` includes them instead of restating the sets. The
  `sphinx-html` target regenerates them, Docs Check fails a change that
  edits a counter without rerunning the script, and both docs workflows
  now trigger on the counter sources, so adding a counter republishes the
  site with it listed.
* `--nic-profile NAME|PATH` (repeatable, later layers win per key) chooses
  what the emulated ionic NIC looks like to the guest: its identity (ASIC
  type and revision, firmware version) and link (speed, transceiver, maximum
  MTU). Profiles are ini files; `compat` (the default, byte-identical to the
  device before profiles) and `small-vm` are built in, and their sources are
  in `profiles/`. `--print-profile` prints the resolved profile and exits, and
  the startup log and stats file name the profile chain and its hash. An
  unknown key or out-of-range value is an error naming the file, line and
  key. Documented in `docs/ionic.rst`.
* The emulated ionic NIC publishes its LIF statistics block to the guest, so
  `ip -s link` and the driver's `ethtool -S` LIF counters are real: packets
  and bytes by unicast, multicast and broadcast, the drops the driver reports
  as `rx_dropped` and `rx_over_errors`, and RDMA payload. The block is
  refreshed every `stats.interval_ms` (profile key, or
  `--stats-interval-ms`, 10 to 60000, default 1000) and cleared on reset.
  `--stats-file` gains a "LIF Statistics" section. Documented in
  `docs/ionic.rst`.
* The ionic NIC's Ethernet queue count is a profile setting, from 1 to 64:
  the `[queues]` keys `eth`, `rdma_eqs` and `intr_vectors` (or `auto`), or
  `--eth-queues`. The device has up to 128 MSI-X vectors (was 32), sized by
  the profile. Every enabled Rx queue is served, with frames steered by a
  flow hash of the IPv4 or IPv6 5-tuple (VLAN and QinQ aware); before, only
  Rx queue 0 received. The `compat` profile still offers 4 queues and 32
  vectors.
* `--inject 'SITE: PATTERN effect EFFECT [status S]'` (repeatable) injects
  faults into the emulated ionic NIC, with tc netem's grammar and the
  deterministic patterns `every N`, `after N [count M]`, `burst N every M`
  and `sequence "..."`. The first site is `stats_vals`, which makes the
  guest's `hw_counters` reads fail on demand (`effect fail`). A malformed
  fault stops the server at startup with an error naming the bad token.
* A P4 match-action engine can be placed between the guest's Ethernet queues
  and the host network backend, matching the programmable datapath of the
  silicon the emulated NIC models. Build it with the `ERNIC_BUILD_P4` CMake
  option, which is off by default, and put it on the wire with `-P` / `--p4`.
  This first step adds the pipeline stage, the two-port model, and the frame
  path in both directions; no program can be loaded yet, so every frame is
  forwarded unchanged and a server started with `--p4` behaves exactly as one
  started without it. The engine coexists with the in-band S3 control plane,
  which still sees guest frames addressed to it first. Documented in
  `docs/p4.rst`; `tests/test_p4_engine.c` (CTest `p4-engine-unit`) covers it.
* The Python test harness can now drive the emulated NIC's Ethernet data
  queues, not just its admin queues: `ernic.eth` posts frames on the Tx
  ring, posts receive buffers, and reaps both completion rings, including
  scatter/gather frames. `tests/py/test_p4.py` (CTest `ernic-p4-driver`)
  uses it to put guest frames through the P4 engine on a real server over
  vfio-user, with no VM and no root. The server reports the engine's frame
  counters in a `P4 stats:` line at shutdown, which is what that test
  asserts on.
* The P4 engine accepts a program in pieces, ready for a transport to carry
  one in: `p4_engine_load_begin()`, `p4_engine_load_chunk()` and
  `p4_engine_load_commit()` accumulate up to `P4_PROGRAM_MAX` bytes, which
  is 4 MiB. Pieces must cover the program in order and without gaps, and
  anything else -- a chunk running past the declared length, one that skips
  forward or rewrites what already arrived, or a commit before the whole
  program has -- abandons the load rather than leaving a partly filled
  buffer. Nothing parses the bytes yet, so a complete load reports
  `ENOSYS`; no program can be run.
* OCP FP8 E4M3 arithmetic for the P4 pipeline, in `src/p4/p4_fp8.c`. P4 has
  no floating-point type -- its base types are bit-vectors -- so fp8 cannot
  be expressed in a P4 program and is provided as an extern the program
  calls, which is how P4 targets implement in-network reduction. It converts
  in both directions and reduces two operand buffers to a single value with
  `add`, `multiply` (a dot product) or `max`, accumulating in float and
  rounding once at the end, as fp8 hardware does. Overflow saturates to
  +/-448 rather than becoming NaN, which differs deliberately from
  `ml_dtypes.float8_e4m3fn`: one overflowed term must not turn a whole
  reduction into NaN. `tests/test_p4_fp8.c` (CTest `p4-fp8-unit`) checks
  every one of the 256 encodings.
* The emulated NIC can reduce two registered memory regions to a single
  value and deliver it to a third, instead of the host reading two buffers
  and writing one. `ionic_datapath_fp8_reduce()` reads both operand regions,
  applies the fp8 extern, and writes the one-byte result out through the
  same path an RDMA WRITE takes, so over the loopback backend it lands in
  whatever region the destination key resolves to. Operand length is capped
  at 4096 bytes, and an unregistered key or a length running past the end of
  a region faults rather than reaching host memory.
  `tests/test_ionic_harness.c` (CTest `ionic-harness-fp8-reduce`) covers it
  across three regions, including the pages of the destination being
  registered out of order. Note that no P4 program is involved yet: the
  reduction calls the arithmetic directly, so which one happens is fixed by
  the caller rather than chosen by a table.

* A compiled P4 program can now be loaded, as far as its header definitions.
  `p4_engine_load_commit()` parses the accumulated bytes as bmv2 JSON --
  the artifact `p4c --target bmv2 --arch v1model` emits -- using json-c,
  which becomes a dependency only when `ERNIC_BUILD_P4` is on. A program
  that uses a feature the engine does not have yet is refused by name, so
  `p4_engine_load_error()` reports which one rather than that the file was
  bad. Parsing is not finished: a well-formed program still ends in
  `ENOSYS`. A program loads and then runs: bmv2's own `loopback.json`
  forwards a frame unchanged. With `--p4` and a loaded program, where a
  frame goes is decided by the program's `egress_spec` rather than by the
  emulator, including sending it back to the port it arrived on -- which is
  what makes a guest frame answerable without a TAP.
  Fields of any width the loader accepts, up to 512 bits, are compared,
  assigned and added correctly, so an IPv6 address behaves like any other
  field.
* `scripts/fetch-p4c.sh` puts a working p4c in a private directory, so the
  P4 programs under `tests/data/p4/` are compiled from their `.p4` sources
  rather than written by hand. Nothing is installed and nothing on the
  system changes. p4lang publishes p4c for Ubuntu up to 23.04 and the
  binary wants a boost library a current Ubuntu no longer carries, so that
  one library is fetched beside it; both artifacts are pinned by SHA-256,
  which is what establishes they are the right ones, since one of the two
  hosts redirects to a mirror a TLS-inspecting proxy will not validate.
* `tests/data/p4/minimal_forward.json` is now compiler output rather than
  hand-written, with its source in `tests/data/p4/src/`. That changed the
  names it carries: p4c qualifies every table and action with the control
  it was declared in, so the program has `IngressImpl.forward` and not
  `forward`. A control plane written against unqualified names matches no
  real artifact.
* `-J` / `--p4-program PATH` loads a compiled P4 program when the server
  starts, so the engine can be given one without a guest or a control
  plane. It implies `--p4`. A program the engine will not run stops the
  server from starting, naming the feature that was used rather than
  reporting that the file was bad, so a bad program is a startup failure
  and never a silently inert pipeline. The file goes in through the same
  chunked path a control plane uses, so there is one route into the engine
  rather than two.
* A P4 program can instantiate and call an extern the target provides,
  which is how fp8 arithmetic will reach a program at all: P4 has no
  floating-point type, so it cannot be written in the language. The loader
  reads `extern_instances`, allow-lists the type against what this target
  actually has, and accepts the `_<Type>_<method>` call form p4c emits for
  `--emit-externs`, and the pipeline executes the call. What the program
  decides is the policy -- whether to act and with which operation -- and
  what it cannot decide is which memory is touched: the operand regions
  come from the work request, not from anything a table entry can name. A
  call made where there is no operation to work on, which is every frame,
  fails rather than being skipped, and an extern that fails fails the
  operation rather than letting it complete with the work not done.
* The P4 pipeline can be run over an RDMA operation rather than a frame,
  which is the first half of putting it on the RDMA path. A program that
  declares a header called `rdma_metadata` can match on the operation's
  opcode, queue pair, length, immediate data and destination key; one that
  does not is an Ethernet program and that path never runs it, so it costs
  nothing. Nothing is parsed and nothing is deparsed, and the only thing
  the pipeline decides is whether to suppress the operation. An
  architecture field declared at the wrong width, or `rdma_metadata`
  declared such that the deparser would put it on a wire, is refused at
  load.
  `p4_engine_process_rdma()` is the entry point.
* A VM lane runs the whole P4 stack from inside a guest.
  `.github/workflows/p4-lane.yml`, called from `system-tests.yml` as
  "P4 Guest-Driven fp8", boots a guest against a server started with
  `--p4-guest-scope load` and no program loaded, builds and loads
  `ernic_p4` there, and then runs `tests/p4_fp8_client.c`: the guest
  pushes a compiled P4 program down through `/dev/ernic-p4`, installs
  one table entry, posts an ordinary two-SGE RDMA write with libibverbs,
  and checks that an elementwise fp8 sum came back.

  Nothing in that program calls the engine's C API, because a guest
  cannot. That is the point: the in-process tests can set work-request
  fields a driver never would, and one of them was hiding a convention
  no guest could satisfy.

  The lane is the only CI that builds the server with
  `-DERNIC_BUILD_P4=ON` and boots a guest; without that flag the server
  has no P4 code and no `--p4*` options at all.
* CI checks the P4 driver patch series. `driver-build.yml` gains a `p4`
  leg alongside `base` and `srq`, which applies the p4 series to the pinned
  kernel ref and then checks it did what it exists to do: the capability
  bit is defined, an auxiliary device named `p4` is created, the lif
  carries a slot for it, and `drivers/ernic_p4` still binds to the
  resulting name. Applying cleanly is not the check -- a series that
  applied and created nothing would leave the module loading and silently
  never probing.
* A guest kernel module, `drivers/ernic_p4/`, binds to that auxiliary
  device and exposes `/dev/ernic-p4`. Through it a program in the guest
  loads a compiled P4 program, installs table entries, and reads the
  engine's counters, with three ioctls that each become exactly one ionic
  admin-queue command. `drivers/ernic_p4/uapi/ernic_p4.h` is the
  userspace header.

  It is a pipe, not a policy: what a guest may do is still the operator's
  decision, made with `--p4-guest-scope` and enforced by the device, and
  a refusal comes back as the device's own status code rather than being
  flattened into an errno. The node is mode 0600, and a host running more
  than one emulated NIC gets `/dev/ernic-p4.N` for the others.

  It builds against a prepared ionic source tree rather than against
  kernel headers alone, because it uses the ionic driver's private
  headers, which no `linux-headers` package ships. Both patch series are
  required and the Makefile refuses a tree lacking the p4 series rather
  than building a module that could never bind. Its `dkms.conf` falls
  back to a path recorded at install time when `IONIC_SRC` is not in the
  environment, so that a DKMS autoinstall on a later kernel upgrade --
  which runs with none of the variables the original install had -- can
  still find the sources instead of failing at exactly the moment the
  module needed rebuilding.
* An optional ionic driver patch series, `ansible/files/patches/p4/`,
  gives a guest an
  auxiliary device to bind a P4 control driver to. Upstream's
  `ionic_auxbus_register()` creates exactly one auxiliary device, for
  RDMA; the patch factors it so a second, named `p4`, is created when the
  device advertises `IONIC_LIF_CAP_P4`. Real hardware never sets that bit,
  so a kernel carrying the patch behaves exactly as it does without it.

  The series is optional and is not applied by default. Build a guest
  driver with it by naming both series:

  ```
  ansible/files/scripts/fetch-ionic-sources.sh \
      --source-dir DIR --kernel-ref v7.2.4 \
      --patches-dir ansible/files/patches \
      --patches-dir ansible/files/patches/p4
  ```

  `fetch-ionic-sources.sh --patches-dir` can now be given more
  than once, and applies the series in the order given. Giving it at all
  still replaces the default rather than adding to it, so an existing
  caller that names one directory is unaffected. A directory that does
  not exist is now an error rather than a series that silently applies
  nothing, and the digest that decides whether a tree is already up to
  date covers the directory names, so dropping a series from a later run
  is seen as a change.
* A guest can now program the P4 engine itself, through ionic device
  commands 70, 71 and 72. Until now a program could only be loaded by the
  operator on the command line, and a table entry could not be installed
  by anyone at all, so the fp8 reduction was unreachable outside a unit
  test. `--p4-guest-scope` is the operator's grant and decides what the
  guest may do: `none` (the default), `read` (identify, enumerate, read
  counters), `write` (also install, modify and delete table entries), or
  `load` (also replace the running program). A guest granted `load`
  decides where every frame goes and which RDMA operations become fp8
  reductions, which is why nothing is granted unless it is asked for.

  The surface is shaped like upstream's `pds_fwctl` -- an endpoint, an
  operation, a scope the device checks, and in/out buffers -- and keeps
  its opcode numbers, but it is not wire-identical: `pds_fwctl` binds to a
  `pds_core.fwctl.N` auxiliary device and wraps each command in
  `PDS_AQ_CMD_CLIENT_CMD`, and rocm-ernic emulates ionic, not pds_core.
  See docs/p4.rst.

  The LIF capability word gains bit 2, `IONIC_LIF_CAP_P4`, advertised
  only when the engine is on and the operator granted a scope, so a guest
  driver can tell whether there is a control surface before it probes
  for one. Bits 0 and 1 (ETH, RDMA) are unchanged, and real hardware
  never sets bit 2.

  Engine counters can also be read at runtime for the first time; until
  now the only way out of the process was one line printed at shutdown.
* The RDMA data path now runs the pipeline, so a loaded P4 program governs
  RDMA operations and the fp8 extern does real work. The pipeline runs
  after the operation's own memory has been checked and before it is
  dispatched, so a program never sees a request that is already a
  protection error.

  The operand convention is the architecture's, not the program's: an RDMA
  write whose gather list is exactly two equal-length entries supplies the
  two operands, and the work request's rkey and address are the
  destination. Each operand is N bytes and so is the result, while the
  work request's own length says 2N -- a driver puts the sum of the
  gather list there and cannot be made to say otherwise. A table entry
  chooses the
  arithmetic and nothing else -- it cannot name a key or an address.

  An action that calls the extern must also `mark_to_drop()`, because the
  extern writes the destination itself and the ordinary transfer would
  land on top of the result. A program that does not is refused with
  `IONIC_STS_LOCAL_QP_OPER_ERR` rather than quietly producing that.

  With no program loaded, or with one that declares no `rdma_metadata`,
  the data path behaves exactly as it does without P4 built at all.
  `tests/data/p4/fp8_policy.p4` is the worked example: a table keyed on
  the opcode turns an ordinary RDMA write into an elementwise fp8 add
  across three memory regions, and changing the one table entry changes
  the arithmetic.
* An action that cannot be carried out now fails the packet instead of
  being skipped. Nothing that runs today can fail, but an extern reaches
  outside the pipeline and will be able to, and a failure there must not
  leave the work silently not done while the request that asked for it
  completes successfully.
* Table entries can be installed, changed and removed, so the same program
  and the same frame can be made to take a different path.
  `p4_engine_update()` takes a record shaped like a P4Runtime update --
  insert, modify or delete, naming a table and an action -- and is the
  single path by which any control plane changes a table, so a
  network-facing client and the guest's own device commands produce the
  same record rather than each having its own way in. Exact, longest-prefix
  and ternary matching are all supported, with the ties broken as P4
  requires. A table's memory is taken on its first entry, not when the
  program loads.
* `ernic_parse_hexstr()` in `src/parse_int.h` parses a bmv2 hexstr: a
  `0x`-prefixed, variable-width hexadecimal value that may be negative and
  may have an odd number of digits, none of which the existing helpers
  accept. The value is decoded big-endian and right-aligned, and the caller
  is told how many bytes the digits needed so it can refuse one wider than
  the field it is destined for instead of truncating it.
* `ansible/playbooks/tutorial-tests.yml` runs examples 1 (RC send/recv) and
  2 (RC RDMA write) from [RDMA-Tutorial](https://github.com/jcxue/RDMA-Tutorial)
  between two guests, server in one and client in the other. It is
  `--tags tutorial` in `site.yml` and `ci-site.yml`, part of `functional`
  in `ci-site.yml`, and runs after the sanity play in the two-VM job of
  `system-tests.yml` and in `ci/jobs/vm-functional.sh`. The examples
  address the peer by InfiniBand LID, so the play applies a patch from
  `ansible/playbooks/files/rdma-tutorial/` that switches them to RoCE GIDs.
  The controller clones the tutorial from GitHub and copies the patched
  sources into the guests, which then need no network access. Upstream's
  5 and 10 million operations take tens of minutes over the emulated NIC,
  so each example runs `ernic_rdma_tutorial_ops` (default 10000) instead.
* The ionic NIC publishes its port (MAC) statistics, the 89 counters
  `ethtool -S` shows as `frames_*` and `octets_*`, which the driver used to
  list as 0: frames and octets by result, by unicast, multicast and
  broadcast, and by size bucket, counted as on the wire. They are published
  with the LIF statistics and cleared on PORT_RESET. A frame dropped for
  want of a receive buffer, or because the mesh inbox is full, counts as
  received and as `frames_rx_dropped`. Documented in `docs/ionic.rst`.
* The ionic NIC's `hw_counters` include the 30 RDMA error counters the real
  firmware reports, such as `req_rx_cqe_err`, `resp_rx_out_of_buffer`,
  `req_rx_rnr_retry_err` and `resp_tx_remote_access_err`, for the device and
  for each QP. A QP's counters start again at 0 when it is re-created.
  `ernic-rdma-errors` (`nix build .#ernic-rdma-errors`, or the CMake target),
  run in a guest, makes each of these errors happen and checks that its
  counters moved.

### Limitations

* **A P4 conversion's destination may not overlap its source**, and the
  device does not check that it does not. The conversion is streamed a
  chunk at a time, so a destination that overlaps the part of the source
  still to be read destroys it before it is used, and the operation
  completes successfully with wrong results from that point on. Widening
  reaches it easily: fp16 to fp32 into the same address writes two bytes
  for every one it has read.

  Unchecked because checking it is not a range comparison -- two keys
  may be registered over the same guest pages, so overlap has to be
  decided on what the keys resolve to -- and the answer that also covers
  that case is to stage the whole source before writing any of it. Until
  then a guest that overlaps the two regions corrupts its own memory
  with its own keys: it reaches nothing it has not registered, and every
  region is still checked for that.

### Changed

* The installed systemd units, launcher, driver pack, `ernicctl`,
  exporter, and `rocm-ernic.env` name each other by the paths of the
  prefix the project was configured with, rather than always
  `/usr/local/...` and, for the driver patches, `/usr/share/rocm-ernic`.
  An install to `/usr/local` is unchanged except for the patches: the
  default `ERNIC_DRIVER_SOURCE` (in `rocm-ernic.env`, `ernicctl` and the
  driver pack) is now `/usr/local/share/rocm-ernic/patches`, where that
  install puts them. Since the paths come from configuring, install to
  another prefix by configuring with it, not with `cmake --install
  --prefix`, which moves the files without changing what they refer to.
* With `-DCMAKE_INSTALL_PREFIX=/usr`, `rocm-ernic.env` installs to
  `/etc/rocm-ernic`, where the service and `ernicctl` read it, instead of
  `/usr/etc/rocm-ernic`.

* The ionic NIC drops a received frame longer than the maximum frame size it
  advertises (`link.mtu_max` + 18 bytes, 9216 in the default profile) before
  the guest sees it, as a real MAC does, and counts it in the port's
  `frames_rx_too_long`, `frames_rx_oversized` and `frames_rx_bad_all`. Before,
  any frame that fit the guest's receive buffer was delivered. This applies to
  frames from the TAP, the mesh and the P4 engine alike, so a sender of jumbo
  frames has to keep within the guest's profile.
* CI, the release build and the Nix flake use libvfio-user 5b9c722 instead
  of 20e9803 (the flake, 323f4cb). Build against 5b9c722 or later: before
  it, the upper half of a 64-bit BAR smaller than 4 GiB, as BAR0 and BAR2
  are, wasn't writable, so a guest couldn't place either BAR above 4 GiB,
  and sizing the BAR reported the wrong size. Before 5d64ebf, unmapping all
  of a client's memory also left libvfio-user's region lookup cache
  pointing at the freed region, so a later DMA to an address in it read
  freed memory and wrote to an unmapped address. The VM lanes' container
  images still carry 8039244, which has the second fix but not the first.
* **Breaking:** the ionic driver patch series and the two scripts that build
  it moved into the Ansible collection: `patches/` is now
  `ansible/files/patches/`, and `scripts/fetch-ionic-sources.sh` and
  `scripts/setup-ionic-dkms.sh` are now under `ansible/files/scripts/`.
  Anything that invoked them by path from a checkout has to be updated; the
  `ERNIC_BUILD_KMOD` CMake targets and the CI jobs in this repository already
  are. The **installed** layout under `share/rocm-ernic` is unchanged, so the
  driver pack, `vm-driver-install.sh` and anything reading the install prefix
  see what they always did.

  This makes a consumer of the `sbates130272.rocm_ernic` collection
  self-contained: the `ernic_srq_dkms` and `ernic_srq_provider` roles now
  default to the series shipped beside them, so building the SRQ-capable
  `ionic`/`ionic_rdma` modules and an rdma-core with `ionic_create_srq` needs
  no second clone of this repository and no `*_patches_dir` or
  `*_scripts_dir` override. Before, the roles came from the collection's pin
  and the patches they applied came from a separately pinned checkout, with
  nothing enforcing that the two agreed — a mismatch whose symptom is a guest
  reporting `max_srq = 0` with every other check green.

  The two scripts' shebang is now `#!/bin/bash -eu` rather than
  `#!/bin/bash`, which is what `ansible-test sanity` accepts for a file
  shipped in a collection. They already ran under `set -euo pipefail`.
* The QEMU-derived PVRDMA resource manager and backend device moved from
  `third-party/qemu/` to `src/qemu/`, with its `hw/rdma/` and `include/`
  subdirectories flattened into it, and `third-party/` is gone. The code
  is no longer treated as vendored: it's compiled with the project's
  warnings, so `ERNIC_WERROR` covers it, clang-tidy, IWYU and codespell
  check it, and it counts toward the coverage floor. Local branches and
  patches that touch it need their paths updated.
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
* The hosted CI starts its guests with `-machine hpet=off`, to avoid an
  occasional hang of the nested guest kernel in early boot, before the ionic
  driver loads. When a guest never answers SSH,
  `scripts/qemu-vm-diagnose.sh` now also samples where each vCPU is,
  reports a QMP command that never returns, and reports a kernel that
  stopped before switching to its final clocksource, with what else to rule
  out before putting it down to that hang.
* At startup, the server removes what's at its `--socket` path only if it's
  a socket, as one left by a server that didn't exit cleanly is. It refuses
  to start, saying why, if anything else is there, a symlink included. It
  used to remove a symlink to a socket, and fail with only
  `vfu_create_ctx() failed` for anything else. It now checks and removes the
  entry relative to its directory, opened once, so changing a directory in
  the path in between can't make a server running as root remove a file
  elsewhere. It also no longer waits 100 ms before creating the socket.
* **Breaking:** The server reads the numbers in its options strictly, and
  refuses to start, saying which option is wrong, if one is malformed:
  * The numbers in `--backend nvmeof:...` and `--backend s3:...` options are
    decimal only. A `0x` prefix used to be read as hexadecimal and a leading
    0 as octal, so `port=010` was port 8; it's now port 10, and `port=0x1145`
    is refused. Addresses (`ip=`) are dotted decimal, with no leading zeros,
    signs, or spaces.
  * `--mac` takes exactly six pairs of hex digits separated by colons, such
    as `02:00:00:00:00:01`, as its usage says. It used to accept single
    digits, and ignore anything after the sixth byte.
  * The verbs backend's `port=` is refused if it has anything but digits;
    `port=1x`, `port=+1`, and `port= 1` used to be port 1.
  * The `tcp` backend's ports (`tcp:manager:listen:PORT`,
    `tcp:manager:HOST:PORT`, and `tcp:worker:HOST:PORT`) are refused if
    they have anything but digits, and a value with a leading space or `+`
    in `ERNIC_TCP_LISTEN_BACKLOG`, `ERNIC_TCP_HEALTH_INTERVAL`, or
    `ERNIC_TCP_SOCKBUF` is ignored, as other malformed values already were.
* The `s3` backend's `size=` accepts a `T` multiplier, as
  `docs/s3.rst` says and the `nvmeof` backend's does.
* A local RC SEND that finds no posted receive now waits, as a real
  requester retries behind an RNR NAK, for up to 5 seconds before failing
  with an RNR-retry-exceeded completion. It used to be dropped at once, with a
  successful completion. UC, UD and GSI sends with no receive are still
  dropped.
* A TCP mesh manager no longer connects to its workers. It closes the
  connection of a node that hasn't answered for three health-check intervals
  (`ERNIC_TCP_HEALTH_INTERVAL`, 5 seconds by default), as it already did one
  whose node hung up, and the worker connects again itself: when its
  connection to the manager ends, or nothing has come on it for three of the
  worker's own intervals, it registers again, asking for its own node id. A
  worker that has heard nothing from the manager for an interval sends it a
  heartbeat, which the manager answers, so the two needn't be set to the same
  interval. The manager gives a worker its id back if it registers from the
  address it registered before and can't still be answering on its old
  connection, and otherwise refuses it, without giving it another id; the
  worker asks again an interval later. The protocol version is unchanged, so
  upgrade a mesh's manager and workers together: a worker from before this
  release never connects to the manager again, and a manager from before it
  gives a worker that asks for its id a new one, which the worker refuses,
  and then stops asking.
* For contributors: wire fields in byte buffers are read and written with
  the `ernic_get_*`/`ernic_put_*` accessors in the new `src/byteorder.h`
  (little- and big-endian, 16, 32 and 64 bits), which replace the copies
  in `nvmeof.h` (`nvme_get_le32()` and the rest are now
  `ernic_get_le32()` and so on), `nvmeof_cm.c`, `s3_token.c`,
  `ionic_eth_emu.c` and the tests, and the `memcpy()` plus `le32toh()`
  sequences in the ionic device code. Use them rather than writing another
  set. The new `byteorder-unit` CTest test checks each against byte images
  written out by hand. `tests/s3_rdma_client.c`, which is built inside a
  guest from the files copied to it, keeps its own.

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
* The `/run-ci` pull request comment command and its `ci-command.yml`
  workflow, which ran on every issue and pull request comment. Start the
  self-hosted CI from the Actions tab's *Run workflow* button, or with
  `gh workflow run self-hosted-ci.yml -f pr=N`; `ci/README.md` describes the
  inputs.
* The PVRDMA DMA-mapping layer in `rocm_ernic_compat.c`, which nothing
  called, and the QEMU compatibility stubs and headers that only removed
  PVRDMA code used. With it went the `ERNIC_DEBUG_DMA_MAP` environment
  variable, and the `ernic_debug_dma_map` Ansible setting that wrote it. Only
  that layer read it, so it already had no effect.

### Fixed

* The server's startup line `Added MSI-X capability at offset` now prints
  the offset in hex. It printed the decimal value after a `0x` prefix, so a
  capability at `0x40` was reported at `0x64`.
* The self-hosted CI's instances (`ci/jobs/vm-up.sh`) now advertise each
  guest's IPv4 address as a GID, as well as the link-local one the launcher
  derives from its MAC. The mesh routes a destination GID only to the
  instance that advertised it, so a test addressing the peer by its
  IPv4-mapped GID, as `vm-functional.sh`'s `rc-pingpong` probe, the
  perftest runs and the RDMA-Tutorial examples do, failed to bring its QP to
  RTR. Each instance's MAC is now pinned to `02:a0:d1:00:00:<n>`, as in the
  hosted two-VM job, so its link-local GID can be named; an `ERNIC_MAC_<n>`
  or `ERNIC_GUEST_GIDS_<n>` already in the environment is kept. The guests'
  subnet is `CI_NIC_SUBNET` (default `192.168.200`).
* The `nvmeof` backend now completes a shutdown that the host requests by
  setting `CC.SHN` with `CC.EN` still set, as the NVMe specification has it,
  and keeps reporting it until a reset clears `CC.EN`. It reported
  `CSTS.SHST` complete only when `CC.EN` was clear, so a host following the
  specification polled until its shutdown timeout: SPDK waits at least ten
  seconds per controller it detaches, which makes twenty for a process that
  probes through the discovery NQN. Enabling the controller clears
  `CSTS.SHST`, including after a shutdown requested with `CC.EN` clear.
* A shutdown of a file-backed `nvmeof` namespace now writes the namespace
  back to the image's storage before the controller reports it complete.
  The controller reports a volatile write cache for such a namespace, and a
  host needn't send Flush before it shuts the controller down, so writes it
  hadn't flushed were left in the host page cache, and lost if the host
  crashed before writing them back itself. If the writeback fails, the
  server logs an error and completes the shutdown anyway. The server's main
  loop waits for it, as it does for Flush.
* A file-backed `nvmeof` namespace (`file=`) is now durable when the guest
  asks for it to be. The controller reported no volatile write cache, so the
  guest never sent Flush or Force Unit Access and a guest's `fsync()`
  completed with the data still in the host page cache; a Flush from another
  initiator only started writeback. The controller now reports a write cache
  for a file-backed namespace and completes Flush, and writes with Force
  Unit Access, only once the data is on the image's storage, failing them if
  writeback fails. A RAM namespace is unchanged. While the host writes back,
  the server's main loop waits; see `docs/nvmeof.rst`.
* `--backend nvmeof:file=PATH` no longer truncates an existing image. Without
  `size=`, an image with data keeps its own size (it was cut or grown to the
  64 MiB default); an image that isn't a whole number of blocks is refused.
  A `size=` smaller than the image is refused instead of shrinking it, and a
  larger one still grows it. A `file=` that isn't a regular file, such as a
  block device, is refused with a message saying so. The server logs the
  size it settled on.
* With `--socket-group`, the server changed the socket's mode by its path,
  following a symlink. In a directory other users can write to, a symlink
  swapped in at the socket's name could make a server running as root give
  another file mode `0660`. The server now changes the group and mode of
  the socket it created through a descriptor. If something else takes its
  place before the server has finished checking it, the server says so and
  exits non-zero. It sets the mode through `/proc/self/fd`, so `/proc` has
  to be mounted.
* If the server opened its `--log-file` but couldn't make it its stdout or
  stderr, it carried on, writing some or all of its log to wherever it was
  started with instead. It now says why on the stderr it inherited and
  exits non-zero. `server-lifecycle` covers it, except in sanitizer builds
  and under valgrind, where it can't set up the failure.
* `--backend tcp`, with no `manager:` or `worker:` role, crashed the server
  with a segmentation fault. The TCP backend now refuses it with "TCP
  backend: No config" and the server exits non-zero. `server-lifecycle`
  covers it.
* The server checks for running out of memory where it copies the
  `--backend` and `--stats-file` arguments and the backend's configuration,
  and exits with an error rather than crashing or running without them. If
  it can't copy the stats file's path, or a string the stats file shows, it
  logs an error rather than silently leaving it out.
* A guest could make the server read and write freed memory. The RDMA
  driver's `RDMA_RESET_LIF` command freed the RDMA admin queue's state, but
  the Ethernet emulator kept a pointer to it until the guest created a new
  admin queue, and an admin queue doorbell in between used it. The pointer
  is now cleared when the state is freed, so such a doorbell is ignored.
  `ionic-harness-rdma-reset` covers it.
* The server no longer touches guest memory after its client goes. When a
  client hangs up, libvfio-user unmaps all guest memory before returning to
  the server, but the server still ran that pass's Rx, statistics, admin
  queue and data path work, which DMA into it. With a libvfio-user before
  5d64ebf, whose region lookup cache could still point at the freed
  mapping, that wrote into freed, unmapped memory and could crash the
  server (seen in CI under valgrind, publishing the LIF statistics); with a
  later one the writes failed. The server now ends the session as soon as
  the context is reset, including after a failed reply, which libvfio-user
  reports as success. `ernic-vfu-client` covers a client that hangs up while
  its command is being answered.
* The server's count of main-loop passes, which it logs when the client
  disconnects or the loop ends, is 64-bit. It was a signed `int`, which
  overflows (undefined behavior) after 2^31 passes: a few days of uptime
  when idle, sooner under load.
* The TCP mesh manager's health check measures how long a node has been
  silent on the monotonic clock. It used the wall clock, so an NTP step or
  a change to the date could have it declare every node dead at once, or
  leave a dead node alive, until the clock caught up. The `--stats-file`
  file is rewritten once a second on the monotonic clock too, which
  `ernic-vfu-client` now checks while a client is attached.
* The server notices a client connecting at once, rather than up to 100 ms
  later, and a worker in the TCP mesh reads a peer's handshake as soon as it
  arrives. Both waited out a fixed sleep before looking again.
* The log level is safe to read from any thread. The first log call looks
  up `ERNIC_LOG_LEVEL`, and two threads making it together raced: both
  could warn about an invalid value, and one could overwrite a level just
  set by `ernic_log_set_level()` with the environment's. The server looks
  the level up before it starts any thread, so it never raced, but code
  that logs first from a worker thread did. `tests/test_log_level.c`
  (CTest `log-level-unit`) runs threads racing the first call in hundreds
  of fresh processes.
* An RDMA admin command longer than one 64-byte ring entry is now read from
  the entries that follow it in the ring, wrapping to the start of the
  admin queue as the Linux driver writes it. The device read on past the
  end of the ring instead, so a `CREATE_QP` posted in the last slot took its
  QP type from whatever guest memory followed the ring. An admin command
  whose length needs more entries than the driver has posted, or more than
  four, now fails instead of running. The device ran the first kind with
  the end of its body taken from its own stack, and took the second kind as
  four entries long, so it read the next command from the middle of it. The
  Linux driver sends neither. `tests/test_ionic_adminq_poll.c` (CTest
  `ionic-adminq-poll-unit`) covers both fixes.
* The P4 engine accepts frames up to the emulated NIC's own 9600-byte
  buffer, rather than 9216. The P4 port claims every frame, so a frame
  the engine refused was a frame the guest sent and nothing forwarded:
  with `--p4` on, every frame between 9217 bytes and the 9582-byte MTU
  the device tells the guest it may set was dropped, silently and only
  in that configuration. A `_Static_assert` now ties the engine's limit
  to the NIC's so they cannot drift apart again.

* The Python test harness's Ethernet rings (`tests/py/ernic/eth.py`)
  track posted buffers by ring slot rather than by posting order, which
  is what an Ethernet completion names. The two agree for the first lap
  of the ring and then diverge: `post_tx()` returned an ever-increasing
  number that no completion could match, and `poll_rx()` read the frame
  out of the oldest buffer ever posted instead of the one it was
  delivered into. Posting over a descriptor that has not completed is
  now refused rather than silently overwriting it. New cases in
  `tests/py/test_p4.py` cross the wrap; the existing Tx case stopped one
  post short of it and so passed either way.

* `scripts/fetch-p4c.sh` now accepts a relative `--prefix`. The prefix is
  written into the generated `bin/` wrappers and the `p4include`
  symlink, both of which are later read from somewhere else, so a
  relative one produced a dangling symlink and wrappers that ran only
  from the directory the fetch happened in. The script's own
  `--version` check runs from that directory, so it reported success.

* The Build and Test workflow installs `libjson-c-dev` unconditionally.
  It was installed alongside meson in the step that builds libvfio-user,
  which runs only on a cache miss, while the configure step requires
  json-c for `-DERNIC_BUILD_P4=ON` on every run. The configure would
  have begun failing on the first run that restored a warm libvfio-user
  cache, which is not the run that introduced it.

* The `ernic_p4_dkms` role now registers `ernic-p4` with DKMS instead of
  building the module by hand and copying it into `/lib/modules`. The
  module is pinned to the symbol CRCs of an out-of-tree ionic that is
  itself DKMS-built, so both must be rebuilt together on a kernel
  upgrade; a hand-copied module was not rebuilt, and after the first
  upgrade it would no longer insert. `dkms.conf` had carried
  `AUTOINSTALL="yes"` all along -- the package was simply never
  registered for it to apply to. The role also removes a module left in
  `/lib/modules/<kernel>/updates/` by an earlier run, which `depmod`
  would otherwise keep preferring over the DKMS one.

  The ionic symbols are now resolved for the kernel being built for,
  rather than by taking the first `Module.symvers` found anywhere under
  `/var/lib/dkms`. On an upgrade that file belongs to the previous
  kernel and its CRCs match nothing, which produces the same
  non-insertable module as using no symbols at all. The lookup lives in
  the new `drivers/ernic_p4/find-ionic-symvers.sh`, which the role and
  `dkms.conf` share.

* `fetch-ionic-sources.sh` now applies patch series given by relative
  path. It changes into the source directory partway through, so a
  relative `--patches-dir` was checked in the caller's directory and
  then matched nothing when the series was applied: the script applied
  no patches at all and reported success. The relative form is what both
  the driver README and this file document, so following either produced
  an unpatched tree and a clean exit. A relative `--source-dir` had the
  same split, writing its sentinel somewhere other than where it had
  looked for it. Both are resolved to absolute paths before anything
  uses them. Callers passing absolute paths, which is what the roles and
  CI do, are unaffected.

* A P4 program declaring a signed field is now refused at load rather
  than loaded as unsigned. Nothing in the engine carries signedness:
  field values are zero-extended and compared with a byte comparison, so
  an `int<8>` holding `0xff` compared greater than zero instead of less.
  The loader names the field, as it does for every other construct it
  does not implement. A field explicitly marked unsigned, which is what
  p4c emits for an ordinary `bit<N>`, is unaffected.
* A P4 program whose action uses a negative literal such as `-0x1` is now
  refused at load. The sign was dropped, so `add_to_field(ttl, -0x1)`, the
  TTL decrement in bmv2's `simple_router.json`, incremented the TTL instead;
  that program no longer loads even with its checksums removed. p4c emits
  a negative literal only for a signed `int<W>` value, which was already
  refused as a signed field, and compiles an unsigned `ttl - 1` to
  `(ttl + 0xff) & 0xff`, so programs it compiles are unaffected.
* A rejected P4 program chunk with a null buffer now abandons the load,
  as every other rejected chunk does and as the interface documents. It
  returned an error without abandoning, so a control plane could retry
  into a half-filled buffer through that one path.
* Guest Ethernet transmit bytes are no longer counted twice when the P4
  engine is on the wire with a host backend attached. A frame the
  pipeline claimed and put back on the wire was counted both where the
  guest queue serviced it and again on the way out, so an unchanged
  frame added twice its length to the transmit byte count. Frames the
  pipeline reflects from the uplink back to the uplink are no longer
  counted as guest traffic either.

* A send work request with more than 14 SGEs, which the 512-byte WQE
  stride the device advertises has room for, is now read whole. The device
  read the first 256 bytes of the WQE and then walked its SGE list as far as
  the stride reached, past the end of the buffer it had read into, so it
  gathered from addresses it took from its own stack. A receive WQE that
  size had the SGEs after its 14th ignored.
* The emulated device now refuses a queue whose geometry it can't honor,
  failing the command that creates it: an event, completion or admin queue,
  or a queue pair's send or receive queue, deeper than 2^24 entries; a send
  or receive queue with a stride outside 2^5 to 2^9 bytes, the largest being
  what the device advertises; and a completion or event queue with a stride
  too small for one entry or above 2^16 bytes. The guest gives these as
  exponents, which the device shifted by unchecked, and a shift by 32 or
  more is undefined behaviour. A completion queue the data path refused
  was also reported to the guest as created. The Linux driver asks for
  nothing outside these bounds.
* A P4 program that the engine refuses no longer leaves a listening socket
  behind while it is being read. `--p4-program` was loaded well after
  `vfu_create_ctx()` bound the socket, so a client connecting in that
  window got a server that was about to exit having given it no reason:
  the socket appearing is the only readiness signal there is. The program
  is now read before the socket exists, and refusing one fails the start
  with nothing published. This also makes the failure deterministic for a
  test that waits on the socket, which is how it was found.
* A guest could provoke undefined behaviour in the server with a single
  `CREATE_QP` admin command. The send and receive ring depths and strides
  arrive in that command and are shifted by -- `1u << depth_log2` when the
  queue pair is registered, and `1u << stride_log2` on the work-request
  path -- but unlike `CREATE_CQ` and `CREATE_SRQ`, `CREATE_QP` never
  bounded them, so any value at or above the width of the type was
  undefined. Both are now bounded and an out-of-range command is refused.
  Only the upper limits are enforced: a small stride describes a useless
  ring rather than a dangerous one, and refusing it would reject commands
  the device has always accepted.
* UndefinedBehaviorSanitizer reports are fatal, as the other sanitizers'
  already were. Without `halt_on_error` it printed a report and carried
  on, so the program exited zero and the test carried on passing -- which
  is how the `CREATE_QP` shift above survived a green sanitizer leg. The
  whole suite passes with it on, so nothing else was hiding behind it.
* A guest could provoke undefined behaviour in the server with one device
  command. `Q_INIT` logged the queue depth as `1 << ring_size` before
  range-checking `ring_size`, which comes straight from the guest's 64-byte
  command, so any value at or above 32 shifted a 32-bit unsigned by more
  than its width. The log line now reports `depth_log2` instead and no
  longer shifts.
* A received frame larger than the receive buffer the guest had posted used
  to wedge the Ethernet receive path permanently. The frame was put back at
  the head of the queue, but the descriptor it did not fit was never
  consumed, so every later pass read the same descriptor, failed the same
  way, and delivered nothing behind it; when the failure was the completion
  write, the payload was also re-DMAed into guest memory on every pass. Such
  a frame is now dropped, as one arriving from the host network backend
  already was, and only a genuinely transient failure -- the guest having
  posted no buffer at all -- is retried.
* A TCP mesh node that was restarted used to be unreachable for the rest of
  the mesh's life. The manager allocates node ids monotonically and never
  evicts a dead node, so the restarted process registered under a new id
  while the dead one kept its claim on the guest's GID; both ids then
  claimed the same address and the duplicate-claim rule resolved it to
  neither, so peers failed `MODIFY_QP` for that destination with "claimed by
  more than one node". A node the manager has declared dead now keeps its
  claims but stops owning them, so it neither answers for its addresses nor
  contests them, and the same guest returning under a new id owns them. A
  node that is merely late gets its claims back when it answers again,
  without having to advertise them, and two *live* claimants still contest
  as before. Restarting one node of a mesh no longer means restarting all of
  them.
* The emulated device now refuses a work request whose address and length
  wrap around the end of the address space before it asks libvfio-user to
  translate them. libvfio-user's fast path adds the two and compares the
  sum against the end of the mapped region, so a large enough length wrapped
  that addition into a small number, which a legitimately mapped region
  passed; the translation that came back then described the full length.
  Unsigned wrap-around is defined behaviour, so no sanitizer reported it.
* An RDMA read or write whose length runs past the end of the region its
  key names is now refused before anything is transferred. The transfer
  walked until it ran out of region and only then reported the overrun, so
  a rejected write had already left its first bytes in the tail of the
  target region and a rejected read had already copied them out. Both
  stayed inside the registered region, so neither reached memory the key
  did not cover. Zero-length operations are still exempt, as the key in one
  is not required to be valid.
* A work request naming memory the guest has not registered now fails on
  the queue pair that posted it, with a local protection error, as InfiniBand
  requires. A SEND whose gather failed used to complete successfully for the
  sender and put the error on the peer's receive completion, so the guest
  was told the wrong thing and the wrong queue pair was blamed; one crossing
  the mesh sent the peer a request it could not satisfy. No data was
  transferred in either case, and none is now.
* A mesh worker that restarts gets its GIDs back. It returns as a brand new
  node -- it asks for an id of `0xFFFFFFFF` every time, and its listening
  port is ephemeral, so nothing in the registration says which node it used
  to be -- while its old registration still stood and still claimed the
  guest's addresses. A GID two nodes claim resolves to neither, so the guest
  came back unreachable and stayed that way: peers addressing it got `no
  mesh node owns destination gid`, nothing retired the stale claim, and a
  manager restart did not clear it either. Rebuilding the whole mesh was the
  only way out.
  The manager now withdraws what an unreachable node claimed, at two points:
  when the health check notices a node has died, which covers one that never
  comes back, and when any worker registers, which covers one that returns
  inside the health-check interval -- the case seen in practice, since a
  restarting container is back long before the five seconds are up. The rule
  is that a node with no live connection cannot be answering for an address,
  rather than any attempt to recognise the returning worker; two *live*
  nodes claiming one GID still contest it, as before. `gsi-mesh` covers the
  restart end to end by destroying the worker device, recreating it, and
  checking its GID follows it to the new node id.
  The withdrawal is reversible: the claims are kept and only stop counting
  towards resolution, so a node that was merely silent for three health-check
  intervals and then answers a heartbeat, or that the health check reconnects
  to, owns its addresses again. Only a node that restarted registers afresh
  and re-advertises; one that did not would otherwise have come back live and
  permanently unreachable.
* A QP entering the Error state now flushes its queues, so a work request
  still outstanding is completed with `IBV_WC_WR_FLUSH_ERR` instead of left
  for a consumer that waits on it forever. `MODIFY_QP` recorded the new state
  and left the send and receive queues alone, and the device had no flush
  path at all; `rping` between two guests therefore exchanged its payloads
  correctly and then hung instead of exiting, because it keeps a receive
  posted and its completion thread sat in `ibv_get_cq_event()` while the main
  thread waited on `pthread_join`. Anything that drains its CQ on teardown
  was exposed to the same hang. It now exits 0 in under a second.
  The device posts one completion per queue rather than one per work request,
  which is the division of labour the driver expects -- the completion names
  no work request, and the driver completes what it has posted from its own
  records, which the device never sees. It goes through the normal CQ path so
  that it raises the CQ event too; the event is the half that unwedges a
  blocked consumer. A QP bound to an SRQ flushes only its send queue, because
  the receives in an SRQ belong to the SRQ and other QPs are still drawing
  from them. Covered by the `qp-flush` and `qp-flush-srq` CTests.
  The flush also cancels the QP's deferred requests -- a READ or atomic sent
  to another instance and waiting on its answer -- before it is published. A
  pending request holds the guest memory its response is to land in, and the
  flush is what lets the consumer complete the work request and reuse that
  memory; the peer is a separate process and nothing here can stop it
  answering late. Without this a response arriving after the flush wrote into
  a buffer that was no longer the request's and posted a second completion for
  a work request the driver had already flushed. Covered by
  `ionic-rq-wrap-unit`.
  The flush is driven only by a `MODIFY_QP` that carries `IB_QP_STATE`,
  matching `rdma_rm_modify_qp()`, so a command changing some other attribute
  no longer flushes a live queue pair on the strength of a state byte the
  driver did not mean.
* A UD or GSI receive is now reported as RoCEv2 over IPv4 when the addresses
  are v4-mapped, instead of being flagged IPv4 while carrying an IPv6 GRH.
  The two disagreed: `IONIC_V1_CQE_RECV_IS_IPV4` was set on every receive
  that carried a source MAC, and the driver turns it into
  `wc->network_hdr_type`, which is the only thing telling a receiver where
  the datagram's addresses are. The kernel reads the 40-byte field through
  `union rdma_network_hdr`, so under that flag the source address is at
  offset 32 and the destination at 36 -- but the GRH kept its GIDs at 8 and
  24. The destination still resolved, by coincidence, because a v4-mapped
  GID carries its IPv4 bytes in its own tail; the source came out as
  `0.0.255.255`, the middle of the destination GID.
  `ib_init_ah_attr_from_wc()` resolves that address with ARP to build the
  reply's address handle, so `cm_req_handler()` blocked for a second,
  returned `-ETIMEDOUT` and dropped the CM REQ without even a reject, and
  guest-to-guest `rdma_connect()` retried forever with nothing logged at
  either end. The device now builds the IPv4 header real hardware delivers
  -- 20 reserved bytes, then an IPv4 header with a valid checksum naming
  UDP -- and derives the completion flag from the bytes it is about to
  deliver, so a locally built header and one forwarded from another mesh
  instance are flagged by the same rule. Datagrams whose GIDs are not
  v4-mapped, such as link-local ones, keep the IPv6 GRH. `ionic-harness-ud`
  and `gsi-mesh` pin the IPv4 form, `ionic-harness-nvmeof` the IPv6 one.
* A server that shut down cleanly removed its socket's path a second time
  after closing the socket, so it could remove the socket of another server
  started at the same path in between.
* The server sized its resource tables from the backend's device limits
  without checking them. A negative limit, or a `max_qp * max_qp_wr` too
  large for the completion context table, gave a table of the wrong size,
  and a table that couldn't be allocated aborted the server. The server now
  refuses to start with an error naming the limits, and reports a failed
  allocation instead of aborting.
* A server that failed to start after creating its vfio-user socket, such as
  one given a `--tap` it couldn't attach, exited with the TCP backend's
  threads still running and left the socket behind. It now tears the device
  down first, which stops and joins those threads and removes the socket, as
  a clean shutdown does.
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
* The `--stats-file` file is mode `0644`, whatever the umask. It was created
  with `0666`, so under a permissive umask any local user could rewrite the
  counters `ernic-exporter` reports.
* The `--stats-file` file is replaced whole on every update instead of being
  truncated and rewritten in place, so `ernic-exporter` and `ernicctl` can no
  longer read it empty or half written. Each update goes to a temporary file
  in the same directory (the stats file's name plus six random characters)
  that is then renamed over it, so the server now needs write permission on
  that directory. A failed write, previously unnoticed, is logged and leaves
  the last good file in place, and "Write count" now counts only updates
  that were published. A symlink at the stats path is replaced, not written
  through. Covered by the new `pvrdma-stats-unit` CTest test.

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
  `goto`, `exit()` or `abort()`. Inside an `if (p)` branch that assigns
  `p` a new value, the `if` no longer counts as a check.
  `analysis-semgrep` tests its rules against the fixtures in
  `nix/analysis/semgrep-tests/` and fails if one doesn't pass, if a rule
  doesn't parse, or if the scan fails, and reports source that semgrep
  could only partly parse. The Nix Check workflow builds the new
  `analysis-runners` check, which tests these targets' scripts against a
  fake server, client and scan.
* `ernic-device-tuple-unit` no longer fails when the build directory's path
  is long. The emulated-device lookup in `tests/ernic_device.h` built each
  sysfs attribute path in a 192-byte buffer and treated a longer one as no
  match; it now allows `PATH_MAX`.
* The `s3` backend read a `Content-Length` of `-1`, or one too large for 64
  bits, on a request with an `x-amz-rdma-token` as the largest possible
  length, rather than refusing the request. It now answers a
  `Content-Length` that isn't just digits with `400`. A `Range` header with
  a sign in it, such as `bytes=--64`, which was read as a huge suffix and
  returned the whole object, is now answered with `416`, as other malformed
  ranges are. `partNumber` is read as decimal; `0x10` used to be part 16.
* The `s3` backend's multipart `uploadId` no longer contains part of a heap
  address from the server process. It is now a per-server counter offset by
  a random value chosen at startup, so an id a client kept from before a
  server restart almost certainly names no upload after it. It is still 16
  hex digits.
* An ionic admin command that fails now carries its status code as the whole
  `status_length` of the completion, as the driver reads it, so the guest's
  log shows `opcode 5 error 3` instead of `error 50331648`. Each statistics
  error has its own status code. The errno the guest sees doesn't change.
* A TCP mesh worker whose connection to the manager dropped was cut off from
  the mesh until it restarted. The manager connected to the worker instead,
  but a worker takes topology updates and GID tables only on the connection it
  opened itself, so it ignored them, couldn't reach a node it had no
  connection of its own to, and kept its old connection in CLOSE_WAIT. It now
  connects to the manager again and registers under its own id (see Changed),
  and takes the mesh's updates on the new connection. A connection to the
  manager that dies without a hang-up, in a partition or a host that stops,
  is noticed too, by either end. A node the manager had declared dead owns its
  GIDs again as soon as it registers again, rather than once it advertises
  them. `ernic-mesh-stress` now also pauses a worker, and then the manager,
  past the health check, on a mesh of its own, and checks that the workers
  register again under their own ids.

### Limitations

### Known issues

* While the TCP backend is running, a mesh node that stops reading can stall
  the node sending to it. A send waits for room in the socket for as long as
  it takes, and a broadcast to every node does that while holding the
  connection table's lock, which the data path needs too. Only shutdown makes
  such a send give up.
* The TCP backend still looks up mesh nodes' hostnames with a lookup that
  can't be interrupted or given a deadline. A worker advertises its
  `gethostname()`, which its peers look up before each connect, and looks up
  the manager's configured host each time it connects to it again, so a slow
  DNS server can make SIGTERM wait for the resolver to give up, which its
  configured timeouts and retries bound. A hostname that resolves from
  `/etc/hosts`, or a numeric address, doesn't wait.
