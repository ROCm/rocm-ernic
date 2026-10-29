# AGENTS.md

Guidance for AI coding agents working in rocm-ernic, a userspace emulated
RDMA NIC for virtual machines built on libvfio-user. See
[README.md](README.md) for scope,
[docs/building.rst](docs/building.rst) for dependencies and CMake options,
[docs/testing.rst](docs/testing.rst) for the test programs, and
[ansible/README.md](ansible/README.md) for provisioning lab guests and
hosts.

## Skills and directory guides

This file holds the rules. How to run the checks is in skills under
[`.claude/skills/`](.claude/skills/), in the Agent Skills format, which
agents that support skills load when a task needs one. If yours doesn't,
read the skill's `SKILL.md` when its task comes up:

- [`ernic-c-checks`](.claude/skills/ernic-c-checks/SKILL.md): the GCC and
  Clang warning builds, clang-tidy, the clang 23 thread-safety build,
  include-what-you-use, and coverage.
- [`ernic-sanitizers`](.claude/skills/ernic-sanitizers/SKILL.md): ASan
  and TSan builds and runs, and looping threaded tests.
- [`ernic-valgrind`](.claude/skills/ernic-valgrind/SKILL.md): memcheck on
  the unit tests and the server.
- [`ernic-lint`](.claude/skills/ernic-lint/SKILL.md): shell, workflow,
  CMake, Python, spelling, docs, and Ansible checks.

Two directories have their own AGENTS.md, which adds to this one. Read it
before changing anything there:

- [`src/rdma/AGENTS.md`](src/rdma/AGENTS.md): the TCP mesh's GID
  resolution, node liveness, broadcasts, eviction, and connection
  lifetime, and the traps in each.
- [`src/p4/AGENTS.md`](src/p4/AGENTS.md): matching what the real P4 tools
  accept and refuse.

## Project direction

rocm-ernic will move into the
[rocm-systems](https://github.com/ROCm/rocm-systems) monorepo and be built
by [TheRock](https://github.com/ROCm/TheRock). Make changes that survive
the move:

- **Keep the defaults safe for a superproject**: e.g. `ERNIC_WERROR` is
  off by default so an untried compiler can't break a downstream build.
- **Don't assume we're the top-level project**: use `PROJECT_*_DIR` or
  `CMAKE_CURRENT_*_DIR`, not `CMAKE_SOURCE_DIR`/`CMAKE_BINARY_DIR`.
- **Namespace what you add**: `ERNIC_` for cache variables, `ernic-` for
  targets, never generic names like `doxygen` or `test_*`.
- **Set flags per target** with `ernic_set_compiler_flags()` and
  `ernic_add_sanitizers()` ([`cmake/`](cmake/)), never globally
  (`add_compile_options()`, `CMAKE_C_FLAGS`).
- **Never remove `-pthread`**: `ernic_set_compiler_flags()` compiles and
  links every program with it because TheRock's sanitizer builds need it,
  even though it looks redundant. A program built without that function
  needs it too.

## Build and test

Match CI:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DERNIC_WERROR=ON \
  -DERNIC_BUILD_P4=ON
cmake --build build
ctest --test-dir build -j"$(nproc)" --output-on-failure
```

- libvfio-user is built from source (CI pins its commit in
  [build-and-test.yml](.github/workflows/build-and-test.yml)); pass its
  prefix with `-DCMAKE_PREFIX_PATH`.
- **Name the build type**: CI never builds the default, RelWithDebInfo.
  Without `ERNIC_WERROR`, warnings don't fail the build.
- `ERNIC_BUILD_P4` (needs json-c) builds `src/p4/` and its tests. CI's
  test, clang-tidy, IWYU, and CodeQL builds turn it on; so must yours.
- Docs build without a C compiler or the C dependencies:
  `-DERNIC_DOCS_ONLY=ON -DERNIC_BUILD_DOCS=ON`.
- **Reuse build directories**, named `build-<config>` in the repository
  root (git ignores them): an incremental build recompiles, and re-runs
  clang-tidy or IWYU on, only what changed.

CI builds GCC 15 and Clang 21 × Debug and Release × no sanitizer, ASan,
and TSan on Ubuntu 26.04, plus a Clang coverage build, and runs the tests
under valgrind in the GCC Debug build. Smoke builds on Ubuntu 24.04 build
and test with GCC 13 (Release) and Clang 18 (Debug). Only Release has
GCC's dataflow warnings, `_FORTIFY_SOURCE=3`, and `NDEBUG`. CI also runs
`tests/test_loopback_ci.sh` and `tests/test_data_transfer` (see the
workflow for their environment).

### Tests

- **Every registered test runs without a VM, RDMA hardware, or root**,
  so a skip means something is wrong, and a test that can't set up fails
  rather than skipping (exit 77). Multi-VM and hardware tests run in the
  `system-tests` workflow and the `*-lane` workflows it calls.
- Build a test program with `ernic_add_test_program()` and register it
  with `ernic_add_unit_test()`
  ([`ErnicTesting.cmake`](cmake/ErnicTesting.cmake)); don't give it
  `-fsanitize` flags. Tests that link the same part of `src/` share a
  source list (`ERNIC_TEST_ADMINQ_SOURCES`) at the top of
  `tests/CMakeLists.txt`.
- A plain build sanitizes nothing, tests included; CI's sanitizer legs
  build with `ERNIC_USE_SANITIZERS` and `ERNIC_USE_THREAD_SANITIZER`.
- **Wait for events, not time**: wait with a deadline for something
  observable (a log line, a counter, a registration), never a fixed
  sleep. For threaded tests, see [below](#testing-threaded-code).
- **Coverage of `src/` must not fall below `MIN_LINE_COVERAGE`** in
  build-and-test.yml, checked on every PR, so test new and changed code,
  error paths included.

## C code

- **C11 for 64-bit Linux**, with no GNU extensions unless you have
  permission. Atomics (`<stdatomic.h>`) and threads (`qemu/thread.h`)
  are available.
- **No bare `__attribute__`.** Use C11's forms (`_Alignas`, `_Noreturn`,
  `_Static_assert`), or a macro from
  [`src/ernic_attributes.h`](src/ernic_attributes.h), which expand to
  nothing for other compilers (`ERNIC_PRINTF_FORMAT()` on any function
  taking a printf format); add one there for a new attribute.
- **Feature-test macros: POSIX.1-2008 plus glibc's `_DEFAULT_SOURCE`**,
  which `ernic_set_compiler_flags()` defines for every target, and
  nothing else. Don't define `_GNU_SOURCE`, `_XOPEN_SOURCE`, or any other
  in a file, header, target, or `CMAKE_C_FLAGS`. (`_GNU_SOURCE` exposes
  every GNU function, turns on glibc's C23 interfaces such as a `strtol()`
  that accepts `0b`, and silences clang-tidy's `android-cloexec-*`.)
  - Call a GNU or Linux extension (`accept4()`, `pipe2()`, `O_PATH`,
    `pthread_setname_np()`) through its wrapper in
    [`src/ernic_gnu.h`](src/ernic_gnu.h). `src/ernic_gnu.c`, the only
    file that defines `_GNU_SOURCE`, holds the wrappers and nothing else;
    add new ones there, and link it into the tests that need it.
  - For `environ`, declare `extern char **environ;` as POSIX requires.
  - The `ernic-feature-macros` test checks this, and the C standard, for
    every file the build compiles, and looks for a `#define` of a
    feature-test macro in the rest.
  - A workflow or playbook compiling a test program in a guest passes
    `-std=c11 -pedantic -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE`, and
    ships and links `ernic_gnu.c` if the program calls a wrapper.
- **Don't `goto` past an initialized declaration into its scope**
  (`-Wjump-misses-init`), as `goto out` to the cleanup does over
  everything declared after it. Declare the variable before the first
  `goto` and assign it where the initializer was, or make a constant
  table `static const`.
- **`/**` comments use Doxygen syntax** (`@param name`, `@p name`), not
  kernel-doc's (`@name:`, `@name`), which `-Wdocumentation-pedantic`
  rejects. Clang checks that each `@param` names a real parameter.
- **Remove a parameter nothing needs.** Where a function pointer's type
  requires one (a backend's ops, a library callback), write
  `(void)name;`, or `-Wunused-parameter` and clang-tidy report it.
- **Include a project header by its path from `src/`**
  (`"pvrdma/rdma_rm.h"`, `"byteorder.h"`), even from the same directory,
  since IWYU spells a header only one way and takes a bare `"rdma_rm.h"`
  for a missing include. The other include roots are `shared/` and
  `src/qemu-compat/include/` (stand-ins for QEMU's headers,
  `"qemu/thread.h"`). Don't add a subdirectory of `src/` to an include
  path.
- **Use explicitly sized types**: `uint32_t`, `uint64_t`, `size_t` for
  sizes, never `long` for a wire or 64-bit value. Write 64-bit constants
  with `UINT64_C()`, not an `L`, `UL`, or `ULL` suffix.
- **Print fixed-width types with `<inttypes.h>` macros** (`"%" PRIu64`,
  `"%#" PRIx64`, `PRIx32`; `%zu` for `size_t`). Never print a `uint64_t`
  with `%lu`, `%lx`, or `%llu`, or cast it to fit one: it's
  `unsigned long` here but not everywhere, so an uncast `%lx` compiles
  cleanly and is wrong elsewhere, and a cast hides the type. `%ld` is
  only for a real `long` (`ftell()`). Cast a signed offset such as
  `loff_t` to `uint64_t` only where it's known non-negative.
- **Parse numbers only with [`src/parse_int.h`](src/parse_int.h)**
  (`ernic_parse_int()`, `_u64()`, `_size()`, `_port()`; `_u64_span()`
  inside a longer string), never `ato*`, `strto*`, `sscanf()`, or a
  hand-rolled loop. Add helpers there, tested in
  `tests/test_parse_int.c`. Guest-side test programs include it too, so a
  VM building one needs a copy.
- **Lay out wire formats as offsets, not structs.** Anything on the wire
  or in guest memory is a byte buffer: give each field an `_OFF` macro and
  the whole a `_LEN`, with a comment drawing the layout and each field's
  byte order (see `nvmeof.h`, `net/net_headers.h`), and access fields
  only with [`src/byteorder.h`](src/byteorder.h)'s `ernic_get_*()`/
  `ernic_put_*()`. Never cast a struct over the buffer, use
  `__attribute__((packed))`, byte-swap a struct's fields in place with
  `htonl()` and friends, or use `<endian.h>` (`htole32()`). Code working
  on a decoded header converts it with a `_put()`/`_get()` pair
  (`tcp_msg_hdr_put()`, `ionic_dp_wire_hdr_put()`), the only code that
  touches its bytes, and a test checks the bytes written against a
  literal. `htons()`/`htonl()` are still right for `struct sockaddr_in`.
  Don't copy `src/p4/p4_devcmd.c`, which predates this.
- **Allocate the way the layer does**, and free with the allocator's
  partner (`g_free()` for anything GLib returns, e.g.
  `g_key_file_get_string()`):
  - `src/pvrdma/`, `src/qemu-compat/`, and `src/rdma/` are built on
    GLib, whose allocators abort when out of memory, so they use
    `g_new0()`, `g_strdup()`, `g_malloc()` unchecked; a `NULL` check
    there is dead code.
  - Except a buffer whose size a guest or peer chooses: `malloc()` and
    check it, so a large request fails alone rather than aborting the
    server (and a test can fail it with `--wrap`). See
    `tcp_recv_message()`, `tcp_queued_msg_new()`, and their tests,
    `ST_NO_MEMORY` in `tests/test_tcp_thread_stop.c` and
    `tests/s3_alloc_fail.c`.
  - Elsewhere, use `malloc()`, `calloc()`, `realloc()`, `strdup()`,
    never GLib's, and check every result.
  - Where a buffer crosses between the two, comment which allocator it's
    from, as `TcpQueuedMsg` and `tcp_recv_message()` do.

## Thread safety

**Races already cause intermittent CI failures**, so thread safety is a
correctness requirement for any change to shared state. The main thread
runs the vfio-user context and poll loop, and only it may DMA into guest
memory. The TCP backend
([`rdma_backend_tcp.c`](src/rdma/rdma_backend_tcp.c) and the `tcp_*.c`
files) adds accept, health-check, and per-connection receive threads
(`qemu_thread_create()`).

- **Anything one thread writes and another reads needs a lock
  (`QemuMutex`) or an atomic**; otherwise it's a data race (UB), and e.g.
  a hoisted stop-flag load hangs the join. `volatile` is not
  synchronization, except `volatile sig_atomic_t` in a signal handler.
- **Comment any memory order weaker than seq_cst**, saying why it
  suffices, like `recv_thread_running` in `rdma_backend_tcp_priv.h`.
- **Document which lock protects each shared field**, next to the field,
  and the order when code takes more than one. The TCP backend's order is
  the `Locks` table at the top of
  [`rdma_backend_tcp_priv.h`](src/rdma/rdma_backend_tcp_priv.h); add a new
  lock there and among `TcpBackendPrivate`'s lock declarations.
- **Don't hold a lock across a blocking call** (socket I/O, sleep,
  `qemu_thread_join()`) or a call that may take the same lock.
- **Every thread must be stoppable and joined**: an atomic stop flag
  polled with a bounded timeout, and a stop that wakes it if it blocks
  (`tcp_stop_accept_thread()` shuts down its socket;
  `tcp_stop_health_check_thread()` signals its condition variable).
- **TSan can't see the guest**, which writes guest-shared memory (rings,
  indices) from another process; choose atomics and fences by reasoning
  about the guest driver.

### Clang's thread-safety analysis

Annotate with
[`qemu/clang-tsa.h`](src/qemu-compat/include/qemu/clang-tsa.h), using
`QemuMutex` (which the analysis knows is a lock), not `pthread_mutex_t`:
`TSA_GUARDED_BY(lock)` on a field, declared after its lock;
`TSA_REQUIRES` on a function whose caller holds a lock; `TSA_EXCLUDES`
on one that takes a lock or joins a thread that does; and
`TSA_ACQUIRED_AFTER` for a new lock's place in the order. Every clang
build checks functions, but only clang 23 checks fields and order in C
(see `ernic-c-checks`).

The analysis can't follow everything:

- It ties a field to another struct's lock only through the same object
  (`conn->priv->lock`, not a separate `priv`).
- It can't follow a conditional lock (`if (sr) lock` ... `if (sr)
  unlock`); pair them, as `ionic_dp_deliver_recv()` does.
- A GLib table's contents are opaque to it, so annotate the pointer.
- Where it can't follow correct code (setup before an object is shared,
  teardown after its threads are joined, an unlocked read the field's
  comment justifies), wrap the smallest region in
  `ROCM_ERNIC_WARN_THREAD_SAFETY_OFF`/`_ON` with a comment saying which,
  or use a helper such as `tcp_conn_sockfd_unlocked()`. A field it can't
  express keeps only its comment, like `MeshNodeInfo`'s.

### Testing threaded code

- **Run TSan** (`ernic-sanitizers`) for any change to shared state or
  threads, and grep its log for reports: a passing test doesn't prove
  TSan was quiet.
- **A flaky test is a suspected race.** Find the race; don't retry,
  lengthen the timeout, or add a sleep.
- **A test must not race the threads it tests.** A thread acting at
  times the test can't observe (a startup pass, a periodic sweep) can
  disturb its setup: stop it first, set up before starting it, or wait
  on something it exposes, such as a counter. Set up only states the code
  can reach (e.g. not a mesh peer connecting to a lower node id, which
  the protocol forbids).
- **Loop new threaded tests so the timing moves**: hundreds of runs in
  one process, pinned to one CPU (`ernic-sanitizers` says how).

## Security

**Security is critical**: the emulator is a guest/host boundary, and a
bug can expose host or other guests' memory or take down a shared
server. Security beats speed and convenience.

- **Treat everything from outside the process as untrusted**: guest BAR
  and doorbell writes, admin commands, descriptors, ring indices, and the
  addresses, lengths, keys, and QP numbers in them; mesh peers; NVMe-oF
  and S3 clients. Check lengths, indices, and ranges before use, and
  every address, key, or length against what was registered. Reject
  malformed input; don't truncate, clamp, or guess.
- **Integer arithmetic must be safe.** Check for wrap before it happens
  (`len > limit || offset > limit - len`, not `offset + len > limit`;
  ask before using `__builtin_*_overflow`). Check ranges before
  narrowing, not casts to silence warnings. No signed overflow, bad shift
  counts, or out-of-range conversions to signed types. No sanitizer
  reports unsigned wrap-around; that's on you.
- **Bound every buffer**: `snprintf()` with a truncation check, never
  `strcpy`, `sprintf`, or `strcat`.
- **`memset` wire data to zero before filling it in**: completions,
  admin responses, anything DMAed to the guest or sent to a peer
  (`= {0}` needn't zero padding). Give new wire structs a
  `_Static_assert` on their size. Valgrind can't check writes into
  guest memory, so this rule is all that covers them.
- **Never guess a mesh peer.** A GID the TCP mesh can't resolve fails
  the command; sending to a default node is a silent write into another
  guest's memory. See [`src/rdma/AGENTS.md`](src/rdma/AGENTS.md).
- **Keep the hardening**: `_FORTIFY_SOURCE`
  ([`ErnicFortifySource.cmake`](cmake/ErnicFortifySource.cmake)),
  `-fstack-protector-strong`, `-fstack-clash-protection`, and from
  `ernic_set_linker_flags()`, PIE, full RELRO, and a non-executable
  stack.
- **Fix security scan findings** from CodeQL
  ([codeql.yml](.github/workflows/codeql.yml)) and the PR scan
  ([pr-security-scan.yml](.github/workflows/pr-security-scan.yml)), add
  none, and ask before dismissing one.

### Patterns the security scanners report

Avoid these everywhere, tests included (they're scanned too), and add
new general patterns here when a scanner reports one.

- **World-writable files or sockets** (`cpp/world-writable-file-creation`,
  e.g. `chmod()` or `open()` with `0666`): grant owner, and group only if
  needed. If it mustn't be reachable even briefly, create it under a
  restrictive `umask()`, as `create_socket()` in `rocm_ernic_server.c`
  does; narrowing it afterwards leaves a window.
- **Check-then-act on a path** (`cpp/toctou-race-condition`: `stat()` or
  `access()`, then `open()`, `unlink()`, ...): act on a descriptor, or
  just do it and check the result. For name-only calls, open the
  directory with `O_PATH` (`O_RDONLY` needs read permission) and use
  `fstatat()`/`unlinkat()`, as `ernic_socket_remove_stale()` does.
- **Running a program named by `getenv()` or `argv`**
  (`cpp/uncontrolled-process-operation`, into `exec*()`, `system()`, or
  `popen()`): run a tool such as valgrind from outside
  (`--trace-children=yes`), and compile in built programs' paths with
  `$<TARGET_FILE:...>`, like `ERNIC_SERVER_PATH` in
  `test_server_lifecycle.c`.
- **A stack address stored where it can outlive its frame**
  (`cpp/stack-address-escape`), even if unused after return: allocate
  the target, as `test_server_lifecycle.c` does for the server's
  `environ`.
- **A peer-sized allocation bounded only through a pointer**
  (`cpp/uncontrolled-allocation-size`): `if (hdr->len > MAX)` before
  `malloc(hdr->len)` still reports. Copy the length to a local, check
  that, and allocate with it, as `tcp_recv_message()` does.
- **Including a `.c` file** (`cpp/include-non-header`,
  `bugprone-suspicious-include`): a unit test needing private symbols
  includes the module's `_priv.h` (e.g. `src/ionic/ionic_adminq_priv.h`)
  and lists the module's `.c` in `tests/CMakeLists.txt`. Only the module
  and its tests include it; prefix its functions with the module name.
- **Ignoring the result of a function a test stubs out**
  (`cpp/useless-expression`): CodeQL resolves a call to every definition
  in any target the file is built into, so a no-op stub in a unit test
  (`ionic_eth_emu_queue_rx_frame()` in `tests/test_ionic_cq_arm.c`) makes
  a bare call in `src/` "have no effect". Check the result, or cast the
  call to `(void)` with a comment saying why, as `dp_s3_tx_frame()` does.
- **`${{ }}` in a workflow `run:` block** (zizmor `template-injection`)
  for any value not fixed in the workflow, even one read from a repo
  file: it's pasted in before the shell parses it. Map it to `env:` and
  use `"$NAME"`, as `driver-build.yml`'s fetch step does.
- **Standard-library XML parsing** (Bandit B314, entity expansion):
  parse with `xml.parsers.expat`, refusing a DOCTYPE from a
  `StartDoctypeDeclHandler` that raises; for a tree, feed its callbacks
  to `xml.etree.ElementTree.TreeBuilder`. Bandit also flags importing
  `xml.etree` (B405) and `subprocess` (B404): mark the import
  `# nosec` with the one rule and why it's safe.

## Checks

**Every change must pass every check for the files it touches; a change
with an outstanding finding isn't finished.** CI runs most of them on
every PR. Locally, run what's likely to fail, below.

Fix the code rather than suppressing a finding. Suppress only the
unavoidable (vendored code, framework quirks), as narrowly as possible,
with a comment saying why.

### What to run locally

The skills have the commands.

- **Always, for a C change** (`ernic-c-checks`):
  - clang-format on the files you touched.
  - A GCC Debug build with `-DERNIC_WERROR=ON`, and all its tests.
  - GCC Release and Clang builds with `-DERNIC_WERROR=ON` (building is
    enough), preferably with the smoke builds' GCC 13 and CI's Clang 21.
  - clang-tidy, in a Debug build.
- **When the change calls for it**, with one compiler, running only the
  affected tests:
  - Shared state or threads: TSan, then the loop for new or changed
    threaded tests (`ernic-sanitizers`). Races are our most common CI
    failure; don't skip this. For a lock, a guarded field, or an
    annotation, also the clang 23 build (`ernic-c-checks`).
  - Pointers, buffers, lengths, or outside input: ASan
    (`ernic-sanitizers`).
  - Wire data, or anything passed to `send()`/`write()`: valgrind
    (`ernic-valgrind`).
  - New error paths or a lot of new code: coverage, if `src/` is near
    the floor (`ernic-c-checks`).
  - Changed `#include`s: IWYU (`ernic-c-checks`).
  - A new test: the revert check in [Review](#review-your-change).
- **Shell, workflows, CMake, Python, docs, Ansible** (`ernic-lint`): the
  checks are quick, so run each for any file of its kind you touch, even
  where CI would: a workflow's `paths:` filter can miss a change, and
  vermin isn't in CI.
- **Leave to CI** the other compiler and build type for each sanitizer,
  clang-tidy in Release, valgrind on every test and server test, and
  coverage otherwise. When CI fails one, reproduce it locally.

### C

- **clang-format 18.1.3 exactly** (CI's version) with
  [`.clang-format`](.clang-format), over every `.c` and `.h`; fix with
  `clang-format-18 -i`:

  ```bash
  git ls-files -z -- '*.c' '*.h' | xargs -0 clang-format-18 --dry-run --Werror
  ```

- **No warnings** with GCC or Clang, Debug or Release, with
  `-DERNIC_WERROR=ON`. The lists are in
  [`ErnicGNUCompilerOptions.cmake`](cmake/ErnicGNUCompilerOptions.cmake)
  and
  [`ErnicClangCompilerOptions.cmake`](cmake/ErnicClangCompilerOptions.cmake).
- **Suppress a warning only with the macros in
  [`shared/rocm-ernic-warnings.h`](shared/rocm-ernic-warnings.h)**, never
  a raw `#pragma`, `-Wno-…`, or a list edit, and only when it's nearly
  impossible to fix (e.g. outside code). For a new one, add a
  `ROCM_ERNIC_WARN_<NAME>_OFF`/`_ON` pair, add both to `StatementMacros`
  in `.clang-format`, and wrap the smallest region. Delete macros left
  unused.
- **Every sanitizer report is fatal.** Sanitizer flags go only in
  [`ErnicSanitizers.cmake`](cmake/ErnicSanitizers.cmake).
- **clang-tidy** ([`.clang-tidy`](.clang-tidy)): every finding is an
  error. `.clang-tidy` enables whole check groups, then disables, marked
  `TODO`, checks the code doesn't pass yet. Never add to that list to
  silence a finding; when the code passes a `TODO` check, delete its line
  and its aliases'. Unavoidable findings get
  `// NOLINTNEXTLINE(check-name)` with a reason.
- **IWYU**: apply its correct suggestions for files you touch;
  `ernic-c-checks` lists the wrong ones to ignore.

### Shell

- **Shebangs**: `#!/usr/bin/env bash` for scripts run from the source
  tree, for systems like NixOS. Scripts installed onto a host or guest
  that may run as root (`service/`, the Ansible collection's `files/`)
  use `#!/bin/bash`, so they don't depend on `PATH`.
  `ansible/files/scripts/setup-ionic-dkms.sh` and
  `fetch-ionic-sources.sh` use `#!/bin/bash -eu`, despite their own
  `set -euo pipefail`, because `ansible-test sanity` accepts no other
  bash form; don't "tidy" it.
- **ShellCheck 0.11.0**, default checks only. Suppress only on the one
  command, with `# shellcheck disable=SCnnnn` and a reason (e.g. SC2329
  for a cleanup function run only by `trap` in a script ending in
  `exit`).
- **Shell embedded in YAML** is checked too: Ansible `shell:`/`raw:`
  tasks, composite actions' `run:` steps, and workflow `run:` blocks.
  GitHub runs a step without `shell:` in a job with `container:` under
  `sh`; keep those POSIX or set `shell: bash`. For a heredoc to be
  checked, run a shell visibly (`'bash -s'`, `bash`, or `ssh` with no
  remote command) and quote the delimiter (`<<'EOF'`) unless the local
  shell must expand something. Prefer a heredoc to a quoted `ssh` command
  string, which isn't checked.

### Python

- Python must run on **3.10** (no `tomllib`, `except*`, `typing.Self`,
  ...).
- black, pylint, and vermin must pass on every `*.py` and extension-less
  python script except `nix/analysis/triage/`. Suppress a pylint finding
  on the one line (`# pylint: disable-next=...`) with a reason.

### Docs and Ansible

- The docs build without a Sphinx warning, and pass doc8 at 80 columns.
- The Ansible collection passes `ansible-lint --strict` and
  `ansible-test sanity`, at the versions
  [ansible-collection.yml](.github/workflows/ansible-collection.yml) pins.

## The QEMU fork in `src/pvrdma/`

`src/pvrdma/` is QEMU's PVRDMA device model core, forked and heavily
rewritten. Upstream deleted it, so it's ours to modify, built and checked
like the rest of `src/`. Keep each file's original copyright lines.
History: [src/pvrdma/README.md](src/pvrdma/README.md).

## Copyright lines

AMD copyright lines carry no year; don't add or "update" one. By
license:

- **MIT** (`CMakeLists.txt`, `flake.nix`, `cmake/`, `docs/`, `ansible/`,
  `ci/`, `.github/`, `prometheus/`, `udev/`, `scripts/`, `service/`,
  `tools/`, `nix/` except `nix/analysis/fuzz/`, and `shared/` headers),
  matching `LICENSE.md`:
  `Copyright (c) Advanced Micro Devices, Inc. All rights reserved.`
- **GPL** (`src/`, `tests/`, `nix/analysis/fuzz/`):
  `Copyright (C) Advanced Micro Devices, Inc.` (capital `(C)`, no "All
  rights reserved"), then the QEMU GPL boilerplate.

Leave third-party copyright lines exactly as they are, years included.
Patches under `ansible/files/patches/` take the licenses of the Linux
ionic and rdma-core files they modify (see `LICENSE.md`); don't add a
notice to them.

## Changelog

**Every change a deployer or user could notice needs a
[CHANGELOG.md](CHANGELOG.md) entry in the same change**: the
guest-visible device, server options and environment variables,
`ernicctl`, service and udev files, ernic-exporter metrics, the mesh
protocol, CMake options, installed files and packages, dependencies, and
notable CI and test additions. Purely internal refactoring needs none.

Add it under `UNRELEASED`, in `Added`, `Changed`, `Removed`, `Fixed`,
`Limitations`, or `Known issues`, written for users like the existing
entries: what changed and what they must do differently. Start
deployment-breaking entries with `**Breaking:**`. The Ansible collection
has its own changelog: add a fragment to `ansible/changelogs/fragments/`
using the sections in `ansible/changelogs/config.yaml`.

## Review your change

**Before calling a change done, review all of `git diff` and any new
files as a reviewer would**, fix what you find, and rerun the
[local checks](#what-to-run-locally). These are what Copilot's PR reviews
ask for most:

- **Is there a changelog entry?** The most common request. File
  permissions, test program arguments, and flaky-test fixes count.
- **Is the fix complete?** Find every place with the same pattern or
  state, and fix it or say why not (e.g. liveness restored on heartbeat
  revival but not on reconnect; a socket mode tightened in the server but
  reset to `0666` by `ernicctl hot-reload`).
- **Does each new test fail without the fix?** Revert the fix in a
  scratch copy and check. Watch for tests that pass either way: a packet
  "rejected" by an already-empty queue, a transfer too short to reach the
  boundary, a log count an earlier phase already satisfied.
- **Does every error path work?** Is everything freed or closed
  initialized on that path, partial setup torn down, every descriptor
  closed, and the new paths covered?
- **Is every shared field read under its lock?** Including reads that
  build a message sent after unlocking.
- **Does it follow this file?** E.g. a `parse_int.h` helper whose range
  fits, byte order matching the other end of the wire, `sh`-compatible
  `run:` blocks in container jobs.
- **Do the words match the code?** Comments, docs, changelog, and PR
  description claim only what the diff does: no check described as
  stronger than it is, no guarantee on one path claimed for all.
