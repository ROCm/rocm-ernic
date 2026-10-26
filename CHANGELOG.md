# Changelog for rocm-ernic

## UNRELEASED - rocm-ernic 0.0.1

### Added

* **An AMD Pensando ionic NIC.** The server presents `1dd8:1002` (subsystem
  `1dd8:5400`), so guests use the upstream Linux `ionic` and `ionic_rdma`
  drivers; the guest-side patch series in `ansible/files/patches/` is built
  into a DKMS package by the `ERNIC_BUILD_KMOD` targets. The data path
  executes the fast-registration work requests `IONIC_V1_OP_REG_MR` and
  `IONIC_V1_OP_LOCAL_INV`, which a stock `nvme-rdma` initiator posts ahead of
  every command. `-T` / `--tap IFNAME` attaches the emulated Ethernet
  interface to a host TAP, giving the guest a routable segment with working
  ARP, ICMP and TCP/IP.
* **NIC profiles.** `--nic-profile NAME|PATH` (repeatable; later layers win
  per key) sets what the guest sees: identity (ASIC type and revision,
  firmware version), link (speed, transceiver, maximum MTU) and queues
  (`[queues]` keys `eth`, `rdma_eqs` and `intr_vectors`, or `--eth-queues`:
  1 to 64 Ethernet queues and up to 128 MSI-X vectors). Profiles are ini
  files; `compat` (the default, identical to the device before profiles,
  with 4 queues and 32 vectors) and `small-vm` are built in, with sources in
  `profiles/`. `--print-profile` prints the resolved profile and exits, and
  the startup log and stats file name the profile chain and its hash. An
  unknown key or out-of-range value is an error naming the file, line and
  key. Every enabled Rx queue is served, steered by a flow hash of the IPv4
  or IPv6 5-tuple (VLAN and QinQ aware); before, only queue 0 received.
  Documented in `docs/ionic.rst`.
* **Device statistics the guest and the exporter can read.**
  * The LIF statistics block, so `ip -s link` and the driver's `ethtool -S`
    LIF counters are real: packets and bytes by unicast, multicast and
    broadcast, drops (`rx_dropped`, `rx_over_errors`), RDMA payload and the
    `rdma_req_*` / `rdma_resp_*` error counters. It is refreshed every
    `stats.interval_ms` (profile key, or `--stats-interval-ms`, 10 to 60000,
    default 1000) and cleared on reset; `--stats-file` gains a "LIF
    Statistics" section.
  * Port (MAC) statistics: the 89 `frames_*` and `octets_*` counters
    `ethtool -S` shows, by result, by unicast, multicast and broadcast, and
    by size bucket, counted as on the wire and cleared on PORT_RESET. A frame
    dropped for want of a receive buffer, or because the mesh inbox is full,
    counts as received and as `frames_rx_dropped`.
  * `hw_counters` under `/sys/class/infiniband/<dev>/ports/1/` for the
    in-tree `ionic_rdma` driver, for the device and for each QP: commands,
    bytes sent and received, RDMA read and write bytes, doorbells, WQEs and
    CQEs, and the 30 RDMA error counters the real firmware reports (such as
    `req_rx_cqe_err` and `resp_tx_remote_access_err`). The LIF identity
    advertises `stats_type`, and the STATS_HDRS, STATS_VALS, QP_STATS_HDRS
    and QP_STATS_VALS admin commands are implemented. A QP's counters
    restart at 0 when it is re-created, and all of them on a device reset.
  * `ernic-exporter` publishes the LIF block as
    `ernic_lif_counter_total{ernic_id,role,counter}`, one series for each of
    the 74 named fields of `struct ionic_lif_stats`. The counter is a label,
    so one the device starts keeping is published without an exporter
    release. The Grafana dashboard gains a **LIF Counters
    (ionic_lif_stats)** row.
  * `docs/monitoring.rst` documents both counter sets and how they compare
    with the silicon. The counter lists in `docs/generated/` are generated
    from the C sources by `tools/gen-counter-docs.py`, and Docs Check fails
    a change that edits a counter without regenerating them.
  * `ernic-rdma-errors` (`nix build .#ernic-rdma-errors`, or the CMake
    target), run in a guest, provokes each RDMA error and checks that its
    counters moved.
* **Fault injection.** `--inject 'SITE: PATTERN effect EFFECT [status S]'`
  (repeatable) uses tc netem's grammar with the deterministic patterns
  `every N`, `after N [count M]`, `burst N every M` and `sequence "..."`.
  The first site is `stats_vals`, which makes the guest's `hw_counters`
  reads fail on demand (`effect fail`). A malformed fault stops the server
  at startup with an error naming the bad token.
* **An NVMe over Fabrics target**,
  `--backend nvmeof[:size=...,bs=...,file=...,nqn=...,ip=...,port=...]`,
  reachable from the guest with a stock `nvme connect -t rdma`, so one VM
  and one server instance are a complete fabric. It answers the IB CM
  exchange on GSI and moves data with RDMA READ and WRITE against the
  guest's memory keys. The namespace is anonymous memory, or a file with
  `file=PATH`. Documented in `docs/nvmeof.rst`.
* **An S3-over-RDMA object store**,
  `--backend s3[:bucket=...,size=...,objects=...,ip=...,port=...,maxpart=...]`.
  Object payloads move by RDMA against the client's registered buffer,
  described by the `x-amz-rdma-token` header cuObject v1.2.0 introduced and
  hipObject sends; the reply carries `x-amz-rdma-reply` and
  `x-amz-rdma-bytes`. The control plane is terminated in band on the
  emulated wire (ARP, ICMP and a minimal TCP, in `src/s3_tcp.c`), so the
  endpoint is a host on the guest's segment and needs no `--tap`. S3
  semantics follow versitygw; nothing is vendored from it. A request with
  no token carries its payload in the HTTP body, so `curl` is a usable
  client. Documented in `docs/s3.rst`.
* **A P4 match-action engine**, modelling the programmable datapath of the
  silicon the NIC emulates. Build it with `-DERNIC_BUILD_P4=ON` (off by
  default; it adds a json-c dependency); without it the server has no P4
  code and no `--p4*` options. Documented in `docs/p4.rst`.
  * **On the Ethernet path.** `-P` / `--p4` places the engine between the
    guest's Ethernet queues and the host network backend. With no program
    loaded every frame is forwarded unchanged. With one, the program's
    `egress_spec` decides where a frame goes, including back to the port it
    came from, which makes a guest frame answerable without a TAP; a
    program that doesn't set it forwards to the other port. The engine
    takes frames up to the NIC's 9600-byte buffer, and the in-band S3
    control plane still sees frames addressed to it first. The server
    prints the engine's frame counters in a `P4 stats:` line at shutdown.
  * **Programs.** The engine runs the bmv2 JSON `p4c --target bmv2 --arch
    v1model` emits. `-J` / `--p4-program PATH` loads one at startup and
    implies `--p4`. A program using a feature the engine lacks, including
    signed fields and negative literals, is refused by name, and the server
    doesn't start. Fields up to 512 bits work. Tables support exact,
    longest-prefix and ternary matching, with ties broken as P4 requires,
    and entries are inserted, modified and deleted through one
    P4Runtime-shaped update, whichever control plane sends it. p4c
    qualifies table and action names with their control
    (`IngressImpl.forward`, not `forward`), so a control plane must use the
    qualified names. `scripts/fetch-p4c.sh` puts a pinned p4c in a private
    directory, changing nothing on the system, and the programs in
    `tests/data/p4/` are compiled from their `.p4` sources with it.
  * **fp8 arithmetic on the RDMA path.** A program that declares an
    `rdma_metadata` header runs on every send work request, after its memory
    has been checked and before it is dispatched, and can match on the
    opcode, QP, length, immediate data and destination key. P4 has no
    floating-point type, so the arithmetic is an extern the program calls
    (`p4c --emit-externs`). The table entry chooses the operation; it can
    never name a key or an address. An RDMA WRITE whose gather list is two
    equal-length entries is an elementwise OCP FP8 E4M3 `add`, `multiply` or
    `max` into the work request's destination; one with a single entry is a
    conversion, `cvt(src_fmt, dst_fmt)`, in either direction between fp32,
    fp16, bf16, fp8 E4M3, fp8 E5M2 and fp4 E2M1 (OCP MX, two to a byte).
    Any other WRITE is declined and dispatched as an ordinary transfer,
    unless the program has already dropped it. An action that calls the
    extern must also `mark_to_drop()`. All regions are local: a peer node's
    rkey is refused. Rounding is to nearest, ties to even. A value too large
    for its format becomes an infinity where the format has one and
    otherwise saturates (E4M3 to +/-448, so one overflowed term can't make a
    whole result NaN), as does a NaN in fp4. `tests/data/p4/fp8_policy.p4`
    and `fp8_convert.p4` are the worked examples.
  * **Programmable from the guest.** `--p4-guest-scope` is the operator's
    grant: `none` (the default), `read` (identify, enumerate, read
    counters), `write` (also change table entries) or `load` (also replace
    the program). A guest granted `load` decides where every frame goes and
    which RDMA writes become arithmetic, which is why nothing is granted by
    default. The guest uses ionic device commands 70 to 72, shaped like
    upstream `pds_fwctl` and keeping its opcode numbers, but not
    wire-identical to it. LIF capability bit 2, `IONIC_LIF_CAP_P4`, is set
    only when the engine is on and a scope is granted; real hardware never
    sets it.
  * **A guest driver.** The optional ionic patch series
    `ansible/files/patches/p4/` creates a `p4` auxiliary device when the
    capability bit is set, and the `drivers/ernic_p4/` kernel module binds
    to it and creates `/dev/ernic-p4` (mode 0600; `/dev/ernic-p4.N` for
    further NICs). Its three ioctls (load a program, change table entries,
    read counters; userspace header `drivers/ernic_p4/uapi/ernic_p4.h`) each
    become one admin command, and a refusal returns the device's own status.
    It builds against an ionic source tree with both patch series applied,
    which `fetch-ionic-sources.sh` produces when given `--patches-dir` once
    for each series; the `ernic_p4_dkms` role registers it with DKMS.
  * **Tested** by `p4-engine-unit` (including `tests/data/p4/`'s
    `reject_cases.json`, each refused for the reason it names, and
    `features.json`: parser `select` with masks, LPM and ternary tables,
    every arithmetic primitive and comparison), `p4-fp8-unit` (every E4M3
    encoding), the `ionic-harness-p4-*` groups (the control surface's every
    refusal among them), `ernic-p4-driver` (frames and the control surface
    over a real vfio-user socket, with its servers under valgrind in the
    valgrind leg), a `p4`
    leg in `driver-build.yml` that applies the patch series on top of the
    base series and checks it creates the auxiliary device, the clang-tidy,
    include-what-you-use and CodeQL workflows and the self-hosted CI's
    build job, which all build with `-DERNIC_BUILD_P4=ON`, and the
    "P4 Guest-Driven fp8" VM lane
    (`p4-lane.yml`), which loads a program from inside a guest through
    `/dev/ernic-p4` and checks the result of an ordinary libibverbs write.
    The lane publishes a `p4cvt` fp16-to-fp8 conversion trend at 4 KiB and
    64 KiB.
* **Server options.** `-L` / `--log-level` and `ERNIC_LOG_LEVEL` select the
  log verbosity (`none`, `error`, `warn`, `info`, `debug`). `-G` /
  `--socket-group GROUP` (a name or number) lets that group connect to the
  vfio-user socket, which is then mode `0660`; the server must run as root
  or be a member, and `/proc` must be mounted. `ERNIC_SOCKET_GROUP` in
  `rocm-ernic.env` passes it to every instance the service starts and to
  `ernicctl hot-reload`, so a QEMU that doesn't run as root can connect,
  and still can after a restart. `ERNIC_BACKEND` in `rocm-ernic.env`
  overrides the backend the launcher picks per instance.
* **The mesh backend can claim and withdraw a node's GIDs while it runs**,
  re-advertising them with the existing `TCP_MSG_NODE_GIDS`, so the
  protocol is unchanged. This is an internal entry point, not yet a
  guest-visible feature: nothing passes a guest's GID-table change to it,
  so `ERNIC_TCP_GUEST_GIDS` is still the only way to declare a guest's
  addresses, and an address not listed there is unreachable.
* **The TCP mesh runs over IPv6.** Every node listens on all IPv4 and IPv6
  addresses with one socket (IPv4 only on a host without IPv6), and a host
  name is tried at each address it resolves to, of either family; before,
  the mesh was IPv4 only. An IPv6 address in a `--backend` config goes in
  brackets, `tcp:worker:[fe80::1%eth0]:5000`. `ERNIC_MANAGER_IP` may be an
  IPv6 address, without brackets: the launcher and `ernicctl hot-reload`
  add them.
* **DEB packages.** `cpack -G DEB` builds `rocm-ernic` (the server, the
  systemd service, `ernicctl` and the guest driver pack, under `/usr`, with
  `/etc/rocm-ernic/rocm-ernic.env` as a configuration file that upgrades
  keep) and `rocm-ernic-exporter`; see Packages in `docs/building.rst`.
  Every night that `develop` changes, the `nightly` prerelease is replaced
  with unsupported development packages for Ubuntu 24.04 and 26.04, after
  they are installed in a clean container and the server, `ernicctl` and
  the exporter are checked; see `INSTALL.md`.
  * `-DERNIC_BUNDLE_VFIO_USER=ON` installs a private copy of the
    libvfio-user the server was linked with, in `<libdir>/rocm-ernic`,
    loaded only by the installed server through a `DT_RPATH` that
    `LD_LIBRARY_PATH` can't override: libvfio-user keeps the soname
    `libvfio-user.so.0` through incompatible changes, so no other copy can
    be trusted to match. It needs `ERNIC_VFIO_USER_LICENSE` set to
    libvfio-user's `LICENSE`, and a library linked with full RELRO.
  * `ERNIC_PACKAGE_RELEASE` sets the release part of the version (default
    `local`; not a value CMake takes as false, such as `0`).
  * An install includes rocm-ernic's licenses, in
    `<prefix>/share/doc/rocm-ernic`.
  * The `ernic-package-deb` CTest test checks each package's files, modes,
    dependencies, configuration file and the server's `DT_RPATH`, when
    `dpkg-dev`, `file` and `readelf` are installed. A change to the install
    rules must update its file lists in
    `tests/package/run-package-test.cmake`.
* **Build options.**
  * rocm-ernic can be built inside another CMake project with
    `add_subdirectory()`, and then leaves the build type to the parent
    (CTest `cmake-subproject`).
  * `-DERNIC_USE_CODE_COVERAGE=ON` (Clang only) builds with source-based
    coverage, and the `ernic-coverage` target reports the line coverage of
    `src/` after `ctest`. A CI coverage leg fails below a floor (now 90%)
    and publishes the README's Coverage badge from `develop`.
  * `-DERNIC_USE_CLANG_TIDY=ON` runs clang-tidy 19 or later (with Clang as
    the compiler) using the new `.clang-tidy`, failing the build on any
    finding; a new workflow runs it with clang-tidy 23, Debug and Release.
  * `-DERNIC_USE_IWYU=ON` prints include-what-you-use suggestions, which
    don't fail the build; a new workflow lists them.
  * Clang's thread-safety analysis checks the locking at compile time. The
    TCP backend, the loopback backend and the ionic data path and Ethernet
    emulator annotate which lock guards each field, which locks a function
    needs held or not, and the order the TCP backend's locks are taken in
    (`src/qemu-compat/include/qemu/clang-tsa.h`). Every Clang build checks
    the functions' annotations; a new Thread safety workflow builds with
    Clang 23, the first to check the fields' and the order in C, and fails
    on any finding.
  * `ERNIC_SANITIZER_SHARED_RUNTIME` selects the shared or static sanitizer
    runtime. It defaults to the compiler's own choice: `ON` with GCC, `OFF`
    with Clang, where `ON` adds an RPATH to the runtime's directory.
* **Tests that need no VM, RDMA hardware or root**, so they run in every CI
  build leg, sanitizers included:
  * `ernic-test-ionic-harness` (`ionic-harness-*`) runs the whole ionic
    device in-process over a fake libvfio-user and drives it as the guest
    drivers do: bring-up, the RDMA admin queue, RC and UD transfers,
    atomics, fast registration, SRQs, CQ events, QP flush, Ethernet, the
    NVMe-oF connect, and the S3 store's RDMA GET and PUT. It raised
    coverage of `src/` from about 51% to 67%.
    Its `admin-dma`, `dp-dma` and `eth-dma` groups refuse guest memory
    accesses one at a time, as libvfio-user does for an address the client
    has unmapped, and `tables` fills the EQ, admin queue and MR tables.
  * `tests/py/` drives real server processes over the vfio-user socket from
    Python, the way a VMM does, with guest memory in a shared `memfd` and
    nothing outside the standard library: `ernic-vfu-client` (the protocol,
    and what a client mustn't get away with), `ernic-ionic-driver` (QPs,
    MRs, work requests and completions), `ernic-ionic-hw-stats` (the four
    statistics commands and the counts they report), `ernic-p4-driver`, and
    `ernic-mesh-stress` (a three-node mesh under traffic, hostile peers, the
    version gate, killed and restarted nodes, paused nodes and a peer that
    stops reading; `--size N` varies the mesh), and `ernic-mesh-flow` (two
    nodes writing to each other flat out, and a peer that floods a node).
    Documented in `docs/testing.rst`.
  * `ernic-test-gsi-mesh` (`gsi-mesh`) joins two whole devices in one
    process over the real TCP mesh and checks GID resolution from both
    sides, including across a worker restart. `ERNIC_TEST_MESH_PORT` moves
    its port.
  * `ionic-ci` (device identity, BAR geometry, shutdown, `--tap`), and unit
    and server tests for the NVMe-oF target (`nvmeof-target-unit`,
    `nvmeof-cm-unit`, `nvmeof-ci`) and the S3 store (`s3-token-unit`,
    `s3-http-unit`, `s3-target-unit`, `s3-tcp-unit`, `s3-ci`).
* **A valgrind leg.** The GCC Debug build without sanitizers also runs the
  tests under memcheck, which finds reads of uninitialized memory,
  including bytes passed to `send()`, that the sanitizers don't; any error,
  or a definite or possible leak, fails the test. Test scripts run the
  server under the command in the new `ERNIC_TEST_SERVER_WRAPPER`
  environment variable, and `test_server_lifecycle` runs under
  `--trace-children=yes`. `tcp-thread-stop-unit` skips its out-of-descriptors
  row under valgrind, detected with `<valgrind/valgrind.h>` if installed.
* **System tests in guests.**
  * `ansible/playbooks/nvmeof-tests.yml` (discover, connect, an `O_DIRECT`
    digest round trip, fio, disconnect), run by the hosted
    `system-test-nvmeof` job, the self-hosted `vm-nvmeof` job, and by hand
    with `--tags nvmeof`.
  * `ansible/playbooks/s3-tests.yml`, which builds the libibverbs client
    `tests/s3_rdma_client.c` in the guest, run by `system-test-s3`, `vm-s3`
    and `--tags s3`. The hosted lane publishes an *S3-over-RDMA GET
    bandwidth* trend and the `S3 1M GET` shield.
  * `ansible/playbooks/tutorial-tests.yml` runs examples 1 (RC send/recv)
    and 2 (RC RDMA write) of
    [RDMA-Tutorial](https://github.com/jcxue/RDMA-Tutorial) between two
    guests, patched to address the peer by RoCE GID rather than LID and
    limited to `ernic_rdma_tutorial_ops` operations (default 10000). It is
    `--tags tutorial`, part of `functional` in `ci-site.yml`, and runs in
    the two-VM job of `system-tests.yml` and in `ci/jobs/vm-functional.sh`.
  * The `ionic-patches` CI job checks that every driver patch still applies
    to the pinned `IONIC_KERNEL_REF`.
* **Nix outputs for a NixOS guest.** `nixosModules.ernic-guest` (enable
  with `virtualisation.ernic.enable`) sets up a `qemu-vm.nix` guest for the
  emulated NIC, and `packages.<system>.ernic-run` starts the server around a
  VM launcher, exporting `ERNIC_SOCKET`. `ernic-run` passes `--backend`,
  `--mac`, `--tap`, `--nic-profile`, `--inject`, `--stats-interval-ms`,
  `--eth-queues`, `--stats-file` and `--log-file` to the server as given
  (`--nic-profile` and `--inject` may repeat, in order); a server that
  rejects one exits before the command runs. The Nix Check workflow tests
  the launchers. Documented in `nix/README.md`. On x86_64, and without
  root, a TAP or RDMA hardware:
  * `nix run .#ionic-vm` boots such a guest to a root shell, and
    `nix run .#run-ionic-guest-test` checks its RDMA port reaches
    `PORT_ACTIVE`.
  * `nix run .#ionic-vm-pair` boots two guests joined by the TCP backend,
    so RDMA between them goes through the two servers.
    `ernic-pair-pingpong` in a guest runs `ibv_rc_pingpong` against the
    other, and `--ssh-port N` forwards ssh to both, on N and N+1.
    `nix run .#run-ionic-pair-test` checks RC ping-pong both ways.

### Changed

* **Breaking:** **The emulated device is always ionic**, `1dd8:1002` with
  subsystem `1dd8:5400`, rather than `1022:8001` (see Removed). A guest
  image built for an older release needs its udev rules updated, and should
  refresh `pci.ids` with `update-pciids` for the current `lspci` name.
* **Guest Ethernet leaves through a host TAP**, so each instance needs its
  own TAP on a shared bridge for guest-to-guest IP. The `ernic_host_setup`
  role creates them from `ERNIC_TAP_PREFIX` and `ERNIC_TAP_BRIDGE`, and
  `ci/runner/install-runner.sh` does the same for the self-hosted CI node.
* **Breaking:** **The vfio-user socket is mode `0600`**, not `0666`, and
  `rocm-ernic-launcher` and `ernicctl hot-reload` no longer open it up.
  Whoever connects drives the device as the guest's QEMU, so any local user
  could take over an instance. The socket is created with this mode
  whatever the umask. QEMU running as the server's user or as root, as with
  `sudo ernicctl vm-launch`, is unaffected; for another user, use
  `--socket-group`. The server refuses to start if the socket ends up with
  other permissions or an ACL (which a default ACL on its directory causes;
  remove it with `setfacl -k` or use another directory).
* **The server checks its `--socket` path before using it.** It removes
  what is there only if it is a socket, acting relative to the directory so
  a path changed in between can't redirect it, and refuses to start, saying
  why, for anything else, a symlink included. It no longer waits 100 ms
  before creating the socket.
* **Breaking:** **Numbers in options are parsed strictly**, and a malformed
  one stops the server, naming the option:
  * The numbers in `--backend nvmeof:...` and `--backend s3:...` are
    decimal only (`port=010` was port 8 and is now 10; `port=0x1145` is
    refused), and `ip=` is dotted decimal with no leading zeros, signs or
    spaces. `s3`'s `size=` accepts a `T` multiplier, like `nvmeof`'s.
  * `--mac` takes exactly six colon-separated pairs of hex digits.
  * Ports for the `tcp` backend must be all digits (`port=1x`, `port=+1`
    and `port= 1` were port 1), and a leading space or `+` in
    `ERNIC_TCP_LISTEN_BACKLOG`, `ERNIC_TCP_HEALTH_INTERVAL` or
    `ERNIC_TCP_SOCKBUF` makes the value ignored, as other malformed values
    already were.
* **Breaking:** **An unknown `--backend` name stops the server.** The name,
  everything before the first `:`, must be exactly `none`, `loopback`,
  `tcp`, `nvmeof` or `s3`. Before, any other name ran the server
  with no backend, after a warning, and one that only started with a
  backend's name was partly taken for it: `loopbackjunk` ran as
  `loopback`, and `s3junk` and `nvmeofjunk` started the S3 or NVMe-oF
  target with no backend under it.
* **The default log level is `warn`**, so the per-operation `INFO:` lines
  are gone in steady state; `--log-level info` restores them. `-v` /
  `--verbose` is `--log-level debug`.
* **Breaking:** **Two exporter gauges lost their `_total` suffix**:
  `ernic_instances_total` is now `ernic_instances` and `ernic_vms_total` is
  `ernic_vms`. Update hand-written dashboards, alerts and recording rules;
  the shipped dashboard is updated. The exporter's cumulative series are now
  `TYPE counter` rather than `gauge`, with unchanged names, so `rate()` and
  `increase()` queries are unaffected.
* **Breaking:** **The ionic driver patches and their build scripts moved
  into the Ansible collection**: `patches/` is now `ansible/files/patches/`,
  and `fetch-ionic-sources.sh` and `setup-ionic-dkms.sh` are under
  `ansible/files/scripts/`. Update anything that runs them from a checkout;
  the installed layout under `share/rocm-ernic` is unchanged. The
  `ernic_srq_dkms` and `ernic_srq_provider` roles now default to the series
  shipped beside them, so a consumer of `sbates130272.rocm_ernic` needs no
  second checkout to build SRQ-capable modules, and can no longer get
  patches that don't match its roles (whose symptom was `max_srq = 0`).
* **Breaking:** **The ionic and `ernic-p4` DKMS packages are built only
  for the kernel they were asked for**, not for every kernel with headers
  installed, where a build that could never succeed failed the whole run
  (and an `ernic-p4` built for a kernel without the patched ionic
  wouldn't insert). DKMS no longer rebuilds either by itself. **After a
  kernel upgrade, run `setup-ionic-dkms.sh` again** (or
  `ernic_guest_setup`, or `ernic_srq_dkms`); for P4, re-run
  `ernic_guest_setup` with `patches/p4` in
  `ernic_guest_ionic_extra_patches`, then `ernic_p4_dkms`.
* **`fetch-ionic-sources.sh --patches-dir` can be given more than once**,
  applying the series in order. Giving it at all still replaces the
  default. A directory that doesn't exist is an error, and dropping a
  series from a later run is seen as a change.
* **Installed files refer to each other by the configured prefix**, not
  always `/usr/local`. The default `ERNIC_DRIVER_SOURCE` is now
  `<prefix>/share/rocm-ernic/patches`. Install to another prefix by
  configuring with it, not with `cmake --install --prefix`. With
  `-DCMAKE_INSTALL_PREFIX=/usr`, `rocm-ernic.env` goes to
  `/etc/rocm-ernic`, where the service and `ernicctl` read it.
* **The device behaves more like the hardware:**
  * A received frame longer than the advertised maximum (`link.mtu_max` +
    18 bytes, 9216 by default) is dropped before the guest sees it, from
    the TAP, the mesh and the P4 engine alike, and counted in
    `frames_rx_too_long`, `frames_rx_oversized` and `frames_rx_bad_all`.
  * A local RC SEND that finds no posted receive waits up to 5 seconds, as
    a requester retries behind an RNR NAK, then fails with
    RNR-retry-exceeded. It was dropped at once with a successful
    completion. UC, UD and GSI sends with no receive are still dropped.
* **TCP mesh: workers own their connection to the manager.** A manager no
  longer connects to workers. It closes the connection of a node that has
  hung up, or that hasn't answered for three health-check intervals
  (`ERNIC_TCP_HEALTH_INTERVAL`, 5 seconds by default), and the worker
  connects again and registers under its own node id. A worker that hears
  nothing for an interval sends the manager a heartbeat, so the two needn't
  use the same interval. The manager gives a worker its id back if it
  registers from the same address and can't still be answering on its old
  connection, and otherwise refuses it; the worker asks again an interval
  later. A manager that has no node with the worker's id (it evicted the
  node, or it has restarted) never will, so that worker stops asking, and
  has to be restarted to rejoin. The protocol version is unchanged, so
  **upgrade a mesh's manager and workers together**.
* **TCP mesh: a node that stops reading no longer stalls the others.** A
  send to such a node used to block, and on the device's main thread that
  froze the whole device, request timeouts included.
  * A send gives up once nothing has gone out for the new
    `ERNIC_TCP_SEND_TIMEOUT_MS` (10000 by default; a manager uses half),
    and closes the connection. Requests still queued on it complete in
    error when their own 30-second timeout runs out.
  * The main thread queues the data path's messages, and a thread per
    connection sends them. A connection queues up to 32 MiB of requests
    (64 MiB with replies); past that, the guest's send queue waits, as on a
    NIC out of transmit credits.
  * Broadcasts don't hold the connection table's lock while sending, and
    heartbeats and their answers jump the queue, so steady traffic can't
    get a node declared dead.
  * A remote request with no connection to its node completes with
    retry-exceeded, counted in `req_rx_implied_nak_seq_err`, rather than as
    a remote access error.
* **TCP mesh: a worker ignores topology updates** that don't come on its
  own connection to the manager, as it already did the GID table.
* **TCP mesh: received RDMA messages aren't copied.** The receive thread
  hands the data path the buffer it read a message into. Before, the data
  path copied every message, and one over 64 KiB (they go up to 16 MiB)
  needed two allocations of its size. The 4 MiB of receive buffers each mesh
  node allocated at startup are gone. A node that runs out of memory for a
  message closes that connection, which is then reconnected; before, the
  server aborted.
* **TCP mesh: running out of memory for an RDMA message the guest sends
  fails that work request.** Before, the server aborted.
* **TCP mesh: the manager's GID updates no longer grow with the square of
  the mesh.** On every registration, advertisement, death and revival, the
  manager sent every worker the GID set of every node, N × N messages in an
  N-node mesh, and sent them from the thread that handled the change, which
  stopped reading its own node meanwhile. It now sends the changed node's
  set to every worker, and the whole table only to a worker that has just
  registered, from a thread of its own that merges the changes made while
  it sends. A worker logs a node's GIDs only when they change. The protocol
  is unchanged.
* **TCP mesh: a node that falls behind slows its peers down.** A node queues
  up to 64 MiB, or 4096 messages, of what its peers send for its main loop
  to apply (see Fixed). When the queue is full, the node stops reading from
  a peer until it has room, so TCP holds the peer back: the peer's send
  queue for the connection fills, and then its guest's send queues wait, as
  on a NIC out of credits. The protocol is unchanged.
  * SENDs waiting for the guest to post a receive stay queued for up to 5
    seconds. Once they fill the queue and keep another message out for half
    a second, those still without a receive fail with RNR-retry-exceeded at
    once, and the node logs how much they held, rather than leave the
    peer's connection unread long enough for the peer to give up on it.
  * With no QEMU connected, nothing empties the queue, so once the main loop
    has gone 2 seconds without polling, a full queue drops what arrives, and
    the next pass logs how many it dropped. Requests among them complete in
    error after their 30-second timeout.
* **libvfio-user 5b9c722 or later is required** (CI, the release build and
  the Nix flake now use it). Before it, a 64-bit BAR smaller than 4 GiB,
  as BAR0 and BAR2 are, couldn't be placed above 4 GiB and sized wrongly;
  before 5d64ebf, unmapping all of a client's memory left a stale region
  lookup cache. The VM lanes' container images still carry 8039244, which
  has only the second fix.
* **The QEMU-derived code moved from `third-party/qemu/` to `src/qemu/`**,
  flattened, and `third-party/` is gone. It is no longer treated as
  vendored: compiler warnings, `ERNIC_WERROR`, clang-tidy, IWYU, codespell
  and the coverage floor all apply. Local branches touching it need their
  paths updated.
* **rocm-xio is built against the ionic provider** (`-DGDA_IONIC=ON
  -DRDMA_CORE_BUILD=OFF`), with the `rdma-core/ionic-gda` patches applied
  to the guest's rdma-core, rather than the `rocm_ernic` GDA backend.
* **Build changes.**
  * The tests are built only when `BUILD_TESTING` is on (the default).
  * The git SHA cache variables are now `ERNIC_GIT_SHA_FULL` and
    `ERNIC_GIT_SHA_SHORT`.
  * The server and tests are linked as PIE, with full RELRO, a
    non-executable stack and `--as-needed --no-copy-dt-needed-entries`,
    whenever the linker supports them, not only where the toolchain
    defaults to it (as Ubuntu's GCC does and upstream Clang doesn't).
  * `ERNIC_USE_SANITIZERS` enables every sanitizer except ThreadSanitizer
    that the compiler supports alongside AddressSanitizer: ASan, LSan,
    UBSan and `float-divide-by-zero`, plus `bounds-strict`,
    `float-cast-overflow`, `pointer-compare` and `pointer-subtract` with
    GCC, or `implicit-conversion`, `local-bounds` and `nullability` with
    Clang. Configuring fails for another compiler, or one that can't build
    with them.
  * Every sanitizer report is fatal, UBSan's included, with no
    `*SAN_OPTIONS` needed; programs set their own defaults, which those
    variables override.
  * Unit tests are sanitized only when `ERNIC_USE_SANITIZERS` or
    `ERNIC_USE_THREAD_SANITIZER` is on, like the server, so a plain build
    no longer needs the sanitizer runtimes.
  * GCC and Clang build with `-Wwrite-strings`, so a string literal is a
    `const char[]` and passing one as a `char *` is a warning (an error
    with `ERNIC_WERROR`). clang-tidy's `readability-non-const-parameter`
    is enabled, which reports a pointer parameter to a built-in type that
    could point to const.
* **Stricter linting in CI.** ansible-lint 26.10.1 runs with `--strict`.
  ShellCheck excludes no checks; suppress one only on its command, with a
  reason. actionlint 1.7.12 checks the workflows (runner labels in
  `.github/actionlint.yaml`), and `tools/check-embedded-shell.py` (needs
  PyYAML) ShellChecks shell inside other files: Ansible `shell:` and `raw:`
  tasks, composite actions, heredocs fed to a shell, and workflow `run:`
  blocks GitHub runs under `sh`.
* **Scripts run from the source tree find bash through `PATH`**
  (`#!/usr/bin/env bash`), so `ctest` and the scripts work on systems such
  as NixOS. Installed service and driver-pack scripts keep `/bin/bash`; the
  two in the Ansible collection use `#!/bin/bash -eu`, which `ansible-test
  sanity` requires.
* **Hosted CI starts guests with `-machine hpet=off`**, avoiding an
  occasional early-boot hang of the nested guest kernel, and
  `scripts/qemu-vm-diagnose.sh` reports more about a guest that never
  answers SSH: where each vCPU is, a QMP command that never returns, and a
  kernel stopped before its final clocksource.
* **For contributors:** read and write wire fields with the
  `ernic_get_*`/`ernic_put_*` accessors in the new `src/byteorder.h`
  (little- and big-endian, 16, 32 and 64 bits; `byteorder-unit`), which
  replace the copies in the NVMe-oF, S3 and ionic code (`nvme_get_le32()`
  is now `ernic_get_le32()`). `tests/s3_rdma_client.c`, built inside a
  guest, keeps its own. `ernic_parse_hexstr()` in `src/parse_int.h` parses
  a bmv2 hexstr, reporting how many bytes the value needed so a caller can
  refuse one wider than its field.

### Removed

* **The PVRDMA-derived device and everything that only served it**: the
  `--legacy` / `--pvrdma` flags and the `-I` / `--ionic` no-op, the
  out-of-tree `rocm_ernic_eth` / `rocm_ernic_rdma` driver in `driver/`, the
  `rocm_ernic` rdma-core provider and its direct-verbs headers,
  `ERNIC_DEVICE_MODE`, `ernic_device_mode`, `CI_ERNIC_MODE`, the device-mode
  annotation in `ci/report/publish-perf.py`, and the milestone benchmark
  tables in `docs/performance.rst` (live numbers are in
  `docs/perf-trends.rst`). With it went:
  * its data path in the `tcp` and `loopback` backends. A mesh node now
    ignores that path's messages instead of acting on them; data for a QP
    with no receive posted used to be kept until the QP was destroyed, so
    any mesh peer could grow a node's memory without bound. The `loopback`
    backend still accepts `mode=` and `md5` and ignores them.
  * its DMA-mapping layer in `rocm_ernic_compat.c`, the QEMU stubs only it
    used, and `ERNIC_DEBUG_DMA_MAP` and the `ernic_debug_dma_map` Ansible
    setting, which already had no effect.
  * the loopback rdma_cm responder, `src/net/rdma_cm_proto.c`; the ionic
    device's rdma_cm doesn't need it.
  * the tests and fuzz harnesses of that code (`tcp-mr-bounds-unit`,
    `loopback-sge-copy-unit`, `rdma-cm-proto-unit`,
    `fuzz_rdma_cm_proto`), and the guest- and peer-reachable bugs found in
    it: memory region bounds checks, scatter-gather sums and page-directory
    counts that could wrap or overrun, regions without a host mapping used
    as if mapped, leaked DMA mappings, and a rejected local RDMA write
    reported as a success.
* **The unused packet parse and checksum helpers** in
  `src/net/net_headers.h`, with `net-checksum-unit` and `fuzz_net_headers`.
* **The `/run-ci` pull request comment command** and `ci-command.yml`.
  Start the self-hosted CI from the Actions tab's *Run workflow* button, or
  with `gh workflow run self-hosted-ci.yml -f pr=N`; see `ci/README.md`.
* **The `ERNIC_IONIC_KERNEL_REF` environment hook** (see Fixed: the driver
  pack's ionic baseline).
* **Breaking:** **The `verbs` backend and the `--device`, `--ethdev` and
  `--port` options.** The backend was never implemented: `--backend
  verbs:...` created the socket and then failed to set up the device, and
  the three options, though listed, always printed the usage and exited.
  `verbs` is now an unknown backend, refused before the socket is created,
  and the options are unknown options. Use `loopback` or `tcp`.

### Fixed

* **Any guest process could read and write all guest memory through the S3
  and NVMe-oF targets.** Key 0 is the ionic driver's reserved DMA key, which
  the device reads as a raw guest bus address, and both targets passed the
  client's key straight through. A process that could reach either target
  could name key 0 in an `x-amz-rdma-token` or a keyed SGL and reach any
  guest page, kernel memory included. Both now refuse it: S3 answers `500`
  and NVMe-oF a data transfer error. NVMe-oF also refuses a data block SGL
  that is neither keyed nor in capsule, which carried no key at all, as an
  invalid SGL descriptor type. Host memory was never reachable.
* **Memory errors a guest could cause in the ionic device.** A malicious or
  buggy guest could make the server:
  * use freed memory, with an admin queue doorbell after `RDMA_RESET_LIF`;
  * read past the end of the admin ring: a command longer than one 64-byte
    entry is now read from the entries after it, wrapping as the Linux
    driver writes it, and one needing more entries than were posted, or
    more than four, fails;
  * gather from its own stack, for a send WQE with more than 14 SGEs (a
    receive WQE that size had its later SGEs ignored);
  * shift by out-of-range amounts, which is undefined behavior, using the
    queue depths and strides in `CREATE_QP`, `Q_INIT` and the other
    queue-creating commands. Queues the device can't honor are now refused,
    and a CQ the data path refused is no longer reported as created;
  * accept an address and length that wrap the address space, which slipped
    past libvfio-user's range check;
  * write part of an RDMA READ or WRITE that runs past the end of its
    region before refusing it (within the region, so no other memory was
    reached); it is now refused before anything moves.
* **Guest lengths and addresses that wrap past 2^32 or 2^64.**
  * An inline SEND claiming more data than its work request holds read on
    past the request into whatever guest memory followed it, with no memory
    region to bound it, which on a user QP needn't be the poster's memory.
    It now completes with `IBV_WC_LOC_LEN_ERR`, as does a send whose SGE
    lengths add up to more than 4 GiB, which went out as a short message.
  * An SGE using the reserved DMA key (lkey 0) that runs past the end of
    the address space completes with a local protection error.
  * A UD or GSI send the device refuses, for any of these reasons or for
    naming memory it hasn't registered, now completes in error, with the
    status an RC send gets. Its completion, by send queue index, carried no
    status, so the refused send looked like it had gone. A datagram the
    destination refuses, or one cut short at 8 KiB, still completes
    successfully.
  * `CREATE_MR`, or a fast registration, of a region that runs past the end
    of the address space is refused, and a region that ends exactly there,
    which was refused, is accepted. A failed fast registration now
    completes with `IBV_WC_MW_BIND_ERR` rather than success, including one
    in a work request too short to hold it, and drops its key's previous
    binding, which it left pointing at guest physical address 0. An
    address inside a one-page region that wraps past 2^64 is refused
    rather than wrapped to the bottom of guest memory.
  * An atomic in a work request too short to hold its operands and result
    SGE, which ran on whatever bytes the device had last read there, now
    completes with `IBV_WC_LOC_QP_OP_ERR` and changes nothing.
  * The NVMe-oF target refuses an in-capsule SGL offset that wraps, as an
    invalid SGL; it read the command itself as the data. `size=` of 2^63
    bytes or more with `file=` is refused with an error saying so.
  * `CREATE_EQ` on an interrupt past the MSI-X table, and an RDMA admin
    queue deeper than 2^16 entries, the most its 16-bit indices count, are
    refused.
* **The server no longer touches guest memory after its client goes.**
  libvfio-user unmaps guest memory when a client hangs up, but the server
  went on with that pass's DMA; with a libvfio-user before 5d64ebf this
  wrote into freed memory and could crash. The session now ends as soon as
  the context resets, including after a failed reply.
* **Guest buffers that cross from one host memory mapping to the next.**
  QEMU can share a guest's RAM with the server in several pieces, so a
  buffer the guest sees as contiguous may span two of them. The admin
  queues, device commands, Ethernet rings and P4 commands failed such a
  transfer; they now copy it piece by piece, as the RDMA data path already
  did. A transfer spread over more than 128 pieces, which every path
  failed, the data path included, is now split until each part fits.
* **RDMA semantics.**
  * A QP entering Error flushes its queues, so outstanding work requests
    complete with `IBV_WC_WR_FLUSH_ERR` and the CQ event wakes a blocked
    consumer; `rping` between guests used to hang on exit. A QP on an SRQ
    flushes only its send queue. The flush cancels deferred READs and
    atomics first, so a late answer can't land in memory the consumer has
    reused, and only a `MODIFY_QP` carrying `IB_QP_STATE` triggers it.
  * A work request naming unregistered memory fails on the QP that posted
    it, with a local protection error. A SEND used to complete successfully
    and put the error on the peer's receive.
  * UD and GSI receives with v4-mapped addresses carry the IPv4 header real
    hardware delivers and are flagged IPv4. The flag and the header used to
    disagree, so the kernel CM resolved a bogus source address and silently
    dropped every CM REQ, and guest-to-guest `rdma_connect()` retried
    forever.
  * An MSI-X interrupt raised while its vector is masked, as every vector is
    after reset, is delivered when the guest unmasks it, by either path,
    rather than lost.
  * A received frame bigger than the guest's posted buffer is dropped. It
    used to wedge Ethernet receive permanently.
  * A failed admin command carries its status in the whole `status_length`,
    so the guest logs `error 3`, not `error 50331648`.
  * A failed RDMA admin command reports why, as an ionic status code the
    guest driver logs with it (`opcode 1 error 13` for an id already in
    use): `EINVAL` for a malformed command, `ERANGE` for a queue geometry
    or region the device can't honor, `ENOENT` for an id it doesn't know,
    `EEXIST`, `ENOSPC` when a table is full, `EIO` when guest memory
    can't be read or written, `ERDMA` when the backend refuses, and
    `EOPCODE` for an unknown opcode. Every one used to be `error 1`
    (`EVERSION`).
  * `CREATE_MR`, `DESTROY_MR`, `DESTROY_CQ`, `DESTROY_QP`, `MODIFY_QP` and
    `QUERY_QP` with a body too short to hold the command fail with
    `EINVAL`, as the other admin commands already did. They used to succeed
    without doing anything.
  * The `rdma_*` counters in the LIF block, in `--stats-file` and DMAed to
    the guest, carry real values; they always read zero.
  * `ibv_query_qp()` reports the QP's real state on every backend. The
    `none` and `tcp` backends reported every QP as RTS, and `loopback` one
    in Error as RTS. It reports the destination QP by the number the guest
    gave it; it used to report the server's internal QPN.
  * A `MODIFY_QP` that fails changes nothing: the QP's state, PSNs, Q_Key
    and destination stay as they were. They used to take the new values
    even though the command failed.
  * The `none` backend gave every QP the same number, so with two QPs,
    commands for one acted on the other and the first could never be
    freed. Each QP now has its own, and the server refuses to create a QP
    whose number another QP still has.
  * The `loopback` backend kept a record of every QP ever created until
    shutdown, so a guest creating and destroying QPs grew the server's
    memory without bound, and a destroyed QP could be paired with a new
    one. A QP's record is now freed when it is destroyed.
  * `CREATE_CQ` and `CREATE_QP` fail when the device has no room left to
    record the new CQ or QP, as `CREATE_MR` already did, and `CREATE_CQ`,
    `CREATE_QP` and `CREATE_MR` fail when they name an id already in use.
    Each used to succeed and leave a CQ, QP or MR that could never be
    destroyed, so a guest could use up the server's resources.
  * `RDMA_RESET_LIF`, which the guest driver sends whenever it loads,
    frees every QP, CQ, MR, SRQ and address handle the driver had made.
    A driver that reloaded, crashed or rebooted with them still in place
    leaked them all until the server restarted, and they stayed live: a
    doorbell could make an old QP read and write its rings, and a peer
    could RDMA WRITE through an old MR, in guest memory that had since been
    reused.
* **TCP mesh: a peer could grow a node's memory without bound.** A node's
  receive threads queued every message its peers sent for the main loop to
  apply, however far behind it was, so a peer that sent faster than that, or
  sent SENDs to a QP with no receive posted, which wait there, could grow
  the node's memory until it ran out. The queue is now bounded (see
  Changed).
* **TCP mesh: data sent after its work request completed in error.** A
  flushed QP's queued requests, and a timed-out request, are taken back,
  including from a connection leaving the connection table, so a WRITE or
  atomic the guest was told failed can't go out. A destroyed QP's queued
  requests still go out; taking them back left RDMA-Tutorial's write
  example waiting for its last WRITE. Their replies are ignored: a READ's
  or atomic's result no longer lands in memory the consumer has had back,
  and a QP created later with the same id is neither sent the old one's
  completions nor, when flushed, takes back the old one's requests.
* **TCP mesh: untrusted peers.** Any host that could reach a node's port
  could:
  * crash a worker with a malformed or truncated handshake (a NULL
    dereference, a read past the end, a double free), or a manager by
    sending a second registration on one connection;
  * have a manager act on, or relay, messages from a peer that hadn't
    registered;
  * keep a dead node alive with heartbeats naming it, or have a completion
    or the data of an RDMA READ delivered to another node by naming it as
    the sender. Heartbeats count for, and answers go back on, the
    connection they came on;
  * change a worker's node id and GIDs with a registration answer, or make
    it drop its links with a topology update. A worker now takes its id
    once, only from its manager, and talks to peers only once it has one;
    it used to introduce itself as node 0. A refusal, or an id of 0 or
    `UINT32_MAX`, fails it at once rather than after 5 seconds;
  * run a manager out of file descriptors with connections that never
    register. They are closed after 5 seconds, at most 64 wait at once, and
    a hung-up or refused peer's connection is closed within a health-check
    interval. A failed `accept()` (such as `EMFILE`) is retried after
    100 ms; the old tight loop logged 295 GB.
* **TCP mesh: shutdown and threads.** SIGTERM no longer hangs on an accept
  thread blocked in `accept()`, a health check sleeping out its interval, a
  peer that withholds a payload or stops reading, or a connect to a peer
  that doesn't answer. The backend stops every thread before freeing state
  (which ThreadSanitizer and LeakSanitizer reported), connections are
  reference counted so none is freed while in use, a thread that failed to
  start is no longer joined, and sends use `MSG_NOSIGNAL`, so a vanished
  peer can't kill the server with SIGPIPE. The health check and the
  registration timeout use the monotonic clock, so an NTP step can't declare
  every node dead. `tcp-mesh-ci` fails a node that exits non-zero or isn't
  gone 30 seconds after SIGTERM.
* **SIGINT and SIGTERM go to the server's main thread.** The server's own
  threads (the TCP backend's) block every signal but those a fault raises,
  so a shutdown signal can no longer land on one of them and interrupt its
  system call. The main thread unblocks SIGINT and SIGTERM at startup, so a
  server started with them blocked (a signal mask survives `exec`) still
  shuts down on them; they used to stay pending, so only SIGKILL stopped
  it.
* **TCP mesh: connections recover.**
  * A message header split across TCP segments no longer drops the
    connection, which could happen under load on a real network.
  * A worker whose connection to the manager drops connects again (see
    Changed) instead of being cut off from the mesh until restarted, and a
    connection that dies silently, in a partition or a host that stops, is
    noticed by both ends.
  * Two workers whose connection drops, or whose peer is reported dead and
    then alive, connect again; meanwhile their traffic goes through the
    manager instead of into the dead connection. A worker connects to its
    peers on a thread of its own, without holding the lock the data path
    needs, giving up on each after one health-check interval and retrying
    later, so an unreachable peer no longer stalls RDMA traffic or
    heartbeats for two minutes.
  * The manager's health check holds the mesh table's lock only to decide
    what to do, so registrations and heartbeats no longer wait on it (they
    could deadlock). A node that hangs up is reported dead within one
    interval rather than three, one that answers again is reported alive at
    once, and topology updates arrive in order, with the GID tables as well
    as with each other. A topology update taken before a node registered
    could arrive after a GID table with the node's claims, and the workers
    dropped them until the next table.
  * A worker listens on the port it advertises. Choosing a free port with a
    throwaway socket raced with other processes, and a failure there had it
    advertise port 0.
* **TCP mesh: a restarted node's guest is reachable again.** A restarted
  node registers under a new id while the dead one still claims its guest's
  GIDs, and a GID two nodes claim resolves to neither, so the guest stayed
  unreachable until the whole mesh was rebuilt. A node with no live
  connection is now declared dead, and stops owning its GIDs (when the
  health check notices, or when any worker registers, which also tells the
  other workers it is dead), and owns them again if it answers; GIDs a
  dead node advertises are its own only once it answers. Two live
  claimants still contest a GID.
* **TCP mesh: more than 64 registrations.** A manager never dropped a node
  from its node table, and a restarted worker registers as a new node, so
  once a mesh had had more than 64 registrations in total, its topology and
  GID broadcasts, which carry 64 nodes, left the rest out, live ones as
  likely as dead: their peer connections were torn down and their GIDs
  stopped resolving. Now a node registering for the first time in a full
  table takes the place of the node that has been dead longest, of those
  whose connection is gone; the workers are told that node owns nothing (a
  worker that wasn't connected to the manager then drops the node's GIDs
  when the manager's next topology update leaves it out), and its id isn't
  given out again. When no node can be evicted (they are all alive, or the
  dead ones are still connected), the manager refuses the new node rather
  than take a 65th, and the worker fails to start, logging `Registration
  failed: No space left on device`. A worker evicted while it was only
  paused or cut off is refused when it comes back, stops trying, logging
  once that the manager has no node with its id, and has to be restarted
  to rejoin.
* **TCP mesh: a worker whose manager's host name ends in `manager`.** Any
  `--backend tcp:` config containing `manager:` was taken for a manager's,
  so `--backend tcp:worker:mesh-manager:5000` failed to start, saying a
  manager config must start with `manager:`. The role is now read only from
  the start of the config.
* **TCP mesh: a malformed address in the config is refused.** The
  `--backend tcp:` address was split at every `:` and anything after the
  port ignored, so `tcp:worker:fe80::1:9000` connected to host `fe80` on
  port 1, and `tcp:worker:host:5000:junk` was taken for `host:5000`. The
  server now refuses to start with such a config, saying what is wrong
  with the address.
* **NVMe-oF backend.**
  * A shutdown requested with `CC.EN` still set, as the specification has
    it, now completes. Hosts polled until their timeout (SPDK waited ten
    seconds per controller). Enabling the controller clears `CSTS.SHST`.
  * A file-backed namespace reports a volatile write cache. Flush and Force
    Unit Access writes complete only after writeback and fail if it fails.
    A controller shutdown also waits for writeback, but logs a failure and
    reports shutdown complete anyway. The server's main loop waits during
    writeback. A RAM namespace is unchanged.
  * `file=` no longer truncates an existing image to the 64 MiB default.
    Without `size=` the image keeps its size; a smaller `size=`, an image
    that isn't a whole number of blocks, or a file that isn't a regular file
    is refused. The server logs the size it settled on.
  * An IPv6 `nvme connect` was accepted whatever address it named. The
    target answers only on its IPv4 `ip=` address, so, as from a Linux
    target listening on IPv4, one is now refused (a CM REJ, invalid service
    ID), even one naming the `ip=` address as `::ffff:a.b.c.d`.
  * A DREQ whose connection ID matched nothing was looked up by the guest's
    QP number instead of the controller's, which it carries, so it was
    answered but released nothing; the controller queue stayed open until
    the guest destroyed the QP. It now releases the connection it names, if
    the guest's connection ID matches too.
  * Controller QP numbers start again from the bottom of their range after
    65,535 connections, and could be handed to a new connection while an
    older one still held them. They now skip any a connection holds.
* **S3 backend.** A `Content-Length` of `-1` or beyond 64 bits on a token
  request was taken as the largest length; anything but digits is now a
  `400`. A signed `Range` such as `bytes=--64` returned the whole object and
  is now a `416`, and `partNumber` is decimal (`0x10` was part 16). A
  multipart `uploadId` no longer leaks part of a heap address: it is a
  counter offset by a random value chosen at startup. A `503` (too many
  multipart uploads in flight) said `503 Unknown` in its status line, and
  now says `503 Service Unavailable`. A bucket name of 64 characters or more
  was cut to 63, so one that began with the configured bucket's name was
  served as that bucket; it is now a `404` `NoSuchBucket`. A `HEAD` of a
  bucket that doesn't exist, or with a key too long, answered with an XML
  error body, which a client reusing the connection read as the start of
  the next response; no `HEAD` response has a body now.
* **Server startup and shutdown.**
  * `--socket-group` changed the socket's mode through its path, so a
    symlink swapped in could make a root server give another file mode
    `0660`; it now works through a descriptor, and the server exits if the
    socket is replaced before it has checked it.
  * A clean shutdown no longer removes the socket path a second time, which
    could remove the socket of another server started there meanwhile.
  * A server that fails to start after creating its socket (such as with a
    `--tap` it can't attach) tears the device down, stopping its threads and
    removing the socket.
  * A `--tap` name longer than 15 characters, the most Linux allows, is
    refused at startup. It used to be cut short, which could attach the
    guest to a different interface.
  * A client that disconnects, or sends something else, during vfio-user
    negotiation no longer takes the server down.
  * A `--log-file` that can't be made stdout or stderr is an error, rather
    than logging some of the output elsewhere.
  * `--backend tcp` with no role is refused, rather than a crash.
  * Out-of-memory when copying arguments and configuration is an error,
    rather than a crash or running without them.
  * Device limits from the backend that would size resource tables wrongly
    are refused, and a failed allocation is reported rather than aborting.
  * Shutting down a `loopback`, `nvmeof` or `s3` server a guest had used
    wrote past the end of the backend's heap state.
  * Shutdown no longer leaks the ionic device, the `nvmeof` `file=` path,
    or the loopback backend's CQ and QP queues and deferred-completion
    queue.
  * The first log call from several threads at once no longer races to
    read `ERNIC_LOG_LEVEL`.
  * A client connecting, and a mesh peer's handshake, are noticed at once,
    rather than after a fixed 100 ms sleep.
  * The main-loop pass count is 64-bit; the signed `int` overflowed after
    2^31 passes, a few days idle.
  * The startup line `Added MSI-X capability at offset` prints the offset
    in hex rather than decimal after `0x`.
* **The `--stats-file` file.** It is mode `0644` whatever the umask (it was
  `0666`, so any local user could rewrite the exporter's counters). It is
  replaced whole through a temporary file in the same directory (the stats
  file's name plus six random characters), so readers never see it half
  written; the server now needs write permission on that directory. A
  failed write is logged and leaves the last good file, "Write count"
  counts only published updates, and a symlink at the path is replaced
  rather than written through. Per-QP lines for `MASKED_ATOMIC_CMP_SWP` and
  `ATOMIC_FETCH_AND_ADD` keep the `" : "` separator, so `ernic-exporter`
  no longer drops them, and the exporter no longer gives a device-level key
  after the per-QP block to the last QP.
* **`ernic-exporter` hung on SIGTERM and SIGINT**, so `systemctl stop
  ernic-exporter` waited out `TimeoutStopSec` (90 s by default) and then
  killed it, and Ctrl-C in a terminal did nothing. It now exits at once,
  with status 0, even while a client holds a connection open without
  sending a request. The nightly package check now stops it with SIGTERM
  and fails if it doesn't exit cleanly.
* **Guest driver tooling.**
  * The driver pack recorded a hardcoded `v7.2.4` as the ionic baseline,
    whatever `IONIC_KERNEL_REF` was; the pin is now installed as
    `share/rocm-ernic/ionic-kernel-ref`, and a pack without it fails rather
    than build from a baseline its patches don't match.
  * `fetch-ionic-sources.sh` applied no patches, and reported success, when
    `--patches-dir` was relative, as the documentation shows it; relative
    `--patches-dir` and `--source-dir` now work.
  * A failed ionic DKMS build reports the right `make.log`, and says when
    the log it found was written for another kernel.
  * The `ernic_p4_dkms` role registers `ernic-p4` with DKMS rather than
    copying it into `/lib/modules`, removes a copy an earlier run left in
    `updates/`, and resolves the ionic symbols for the kernel being built
    for (`drivers/ernic_p4/find-ionic-symvers.sh`). A module built against
    another kernel's `Module.symvers` would not insert. The lookup, which
    may rebuild ionic, runs only when the module is built, not whenever
    DKMS reads its `dkms.conf` (`dkms status`, every autoinstall pass).
  * The self-hosted CI's instances advertise each guest's IPv4-mapped GID
    as well as its link-local one, so tests addressing a peer by IPv4 can
    bring their QPs to RTR. Each instance's MAC is pinned to
    `02:a0:d1:00:00:<n>`, and the guests' subnet is `CI_NIC_SUBNET`
    (default `192.168.200`).
* **P4 engine.**
  * With a program loaded, a frame the program didn't route was looped
    back into the guest's own receive ring, so loading a program cost the
    guest its network; it now goes to the other port.
  * A program that selected an fp8 operation broke every ordinary RDMA
    WRITE on the device; one that dropped an operation before calling the
    extern had it dispatched anyway; and one calling the extern twice could
    have an ordinary transfer overwrite the result.
  * Frames between 9217 bytes and the 9582-byte MTU were dropped with
    `--p4` on.
  * Signed fields and negative literals were loaded as unsigned (bmv2's
    `simple_router.json` incremented the TTL), and a rejected chunk with a
    null buffer didn't abandon the load.
  * Transmit bytes were counted twice with a host backend attached.
  * A refused `--p4-program` left a listening socket while the server was
    about to exit.
  * An out-of-range `enum p4_fmt` was narrowed before it was checked
    (not reachable from a guest).
  * `scripts/fetch-p4c.sh` accepts a relative `--prefix`.
  * A parser `select` on a field that isn't a whole number of bytes, such
    as a VLAN ID, matched none of the values p4c writes: the key now gives
    each field whole bytes of its own, as bmv2 does.
  * A literal wider than the field it's assigned to, and a table's default
    argument wider than its parameter, are refused at load rather than
    truncated on every packet. A table entry's arguments and key fields are
    checked against their width in bits, not bytes, so `0x3ff` no longer
    fits a 9-bit port.
  * Two table entries that match the same packets, differing only in a bit
    under an LPM prefix or outside a ternary mask, or in a byte past the
    value's length, are the same entry: the second is refused as a
    duplicate rather than installed where no lookup reaches it.
  * Load errors say what's wrong: an empty or truncated program says so, a
    field width given as a string other than `"*"` is a bad width rather
    than variable-length, a bad `base_default_next` is named as such, and
    committing an incomplete load says how much arrived instead of
    repeating an earlier load's reason.
* **Test and analysis tooling.**
  * `p4-fmt-unit` was built without the project's warnings, sanitizers and
    coverage instrumentation.
  * `tcp-thread-stop-unit`'s watchdog allows six times longer under
    valgrind, where it failed the valgrind leg intermittently, and on a
    timeout prints the threads created, joined and exited.
  * `ernic-device-tuple-unit` no longer fails in a build directory with a
    long path.
  * The Nix `analysis-sanitizers`, `analysis-tsan` and `analysis-valgrind`
    targets fail when the server didn't get through the exercise, rather
    than report a clean run; `summary.txt` starts with `exercise: clean` or
    `exercise: incomplete (<reason>)`.
  * The `malloc-no-null-check` and `goto-usage` rules of
    `analysis-semgrep` never parsed and now run; the target tests its rules
    against fixtures in `nix/analysis/semgrep-tests/` and fails if one
    doesn't parse or pass.
  * RDMA-Tutorial's write example, in the two-VM lane, waits for both ends
    of a message before clearing it. Taking it as arrived on either end let
    the rest of a WRITE land after the clear and count as the next message,
    which could leave the server waiting until `timeout` killed it.
  * The VM lanes boot a guest image (`ci/guest-image.env`) with
    `unattended-upgrades` removed. In the old one it could start as a lane
    began provisioning and hold the dpkg lock past the role's 300-second
    wait, failing the lane.
  * The two-VM sanity and performance plays, and the self-hosted
    `vm-functional` job, look up each guest's IPv4-mapped GID index with the
    new `scripts/find-ipv4-gid-index.sh` instead of passing index 1 to
    perftest and `ibv_rc_pingpong`. The index differs between guests: one
    had its link-local GID at 1, so the two ends used different address
    families and the QP never reached RTR. A guest without the GID now fails
    the play with its GID table. The examples in `docs/testing.rst` look the
    index up too.

### Limitations

* **A P4 conversion's destination must not overlap its source**, and the
  device doesn't check. The conversion streams a chunk at a time, so an
  overlapping destination corrupts the source before it is read, and the
  operation completes successfully with wrong results; widening in place
  does it easily. Checking needs more than comparing ranges, since two keys
  can cover the same guest pages. A guest can only corrupt memory it
  registered itself.

### Known issues

* The TCP backend still looks up mesh nodes' hostnames with a lookup that
  can't be interrupted or given a deadline. A worker advertises its
  `gethostname()`, which its peers look up before each connect, and looks up
  the manager's configured host each time it connects to it again, so a slow
  DNS server can make SIGTERM wait for the resolver to give up, which its
  configured timeouts and retries bound. A hostname that resolves from
  `/etc/hosts`, or a numeric address, doesn't wait.
