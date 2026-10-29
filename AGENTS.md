# AGENTS.md

Guidance for AI coding agents working in rocm-ernic, a userspace emulated
RDMA NIC for virtual machines built on libvfio-user. See
[README.md](README.md) for scope,
[docs/building.rst](docs/building.rst) for dependencies and CMake options,
[docs/testing.rst](docs/testing.rst) for the test programs, and
[ansible/README.md](ansible/README.md) for provisioning lab guests and
hosts.

## Project direction

rocm-ernic will move into the
[rocm-systems](https://github.com/ROCm/rocm-systems) monorepo and be built
by [TheRock](https://github.com/ROCm/TheRock). Make changes that survive
the move:

- **Ask before changing link options**: linker flags
  (`ernic_set_linker_flags()`, `target_link_options()`), `-pthread`, or
  static versus shared linking. What builds and tests fine here can break
  TheRock. Other CMake changes are fine, within these rules.
- **Keep the defaults safe for a superproject**: e.g. `ERNIC_WERROR` is
  off by default so an untried compiler can't break a downstream build.
- **Don't assume we're the top-level project**: use `PROJECT_*_DIR` or
  `CMAKE_CURRENT_*_DIR`, not `CMAKE_SOURCE_DIR`/`CMAKE_BINARY_DIR`.
- **Namespace what you add**: `ERNIC_` for cache variables, `ernic-` for
  targets, never generic names like `doxygen` or `test_*`.
- **Set flags per target** with `ernic_set_compiler_flags()` and
  `ernic_add_sanitizers()` ([`cmake/`](cmake/)), never globally
  (`add_compile_options()`, `CMAKE_C_FLAGS`).

Sister projects:
**hipObject** ([ROCm/hipObject](https://github.com/ROCm/hipObject)), an
RDMA-accelerated S3 client for AMD GPUs, whose CI runs on our ionic NIC
and whose protocol our S3 target speaks ([docs/s3.rst](docs/s3.rst)); and
**hipFile**, GPU-direct file I/O in rocm-systems
([`projects/hipfile`](https://github.com/ROCm/rocm-systems/tree/develop/projects/hipfile)),
which already builds with TheRock and is our model for the move
(`cmake/ErnicSanitizers.cmake` follows it). Borrow ideas freely either
way, but ask before creating a to-do file for, or editing, either one.

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

CI builds GCC and Clang × Debug and Release × no sanitizer, ASan, and
TSan, plus a Clang coverage build, and runs the tests under
[valgrind](#valgrind) in the GCC Debug build. Only Release has GCC's
dataflow warnings, `_FORTIFY_SOURCE=3`, and `NDEBUG`. CI also runs
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
- A plain build sanitizes nothing, tests included; see
  [Sanitizers](#sanitizers).
- **Wait for events, not time**: wait with a deadline for something
  observable (a log line, a counter, a registration), never a fixed
  sleep. For threaded tests, see [below](#testing-threaded-code).
- **Coverage of `src/` must not fall below `MIN_LINE_COVERAGE`** in
  build-and-test.yml, checked on every PR, so test new and changed code,
  error paths included. To measure: Clang, `-DCMAKE_BUILD_TYPE=Debug`
  (as CI, so lines match the source), `-DERNIC_USE_CODE_COVERAGE=ON`,
  `ctest`, then build `ernic-coverage`, which writes
  `coverage/report.txt` and `coverage/lines.txt`. Profiles accumulate;
  delete `coverage/profraw` to start over.

## C code

- **C11 for 64-bit Linux**, with no GNU extensions unless you have
  permission, and no `_FILE_OFFSET_BITS` or `_TIME_BITS`. Atomics
  (`<stdatomic.h>`) and threads (`qemu/thread.h`) are available.
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
- **No BSD `<err.h>`**: `fprintf()` the message and `strerror(errno)`,
  as `warn_errno()` in `rocm_ernic_server.c` does.
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
  for a missing include. The other include roots are
  `shared/` and `src/qemu-compat/include/` (stand-ins for QEMU's
  headers, `"qemu/thread.h"`). Don't add a subdirectory of `src/` to an
  include path.
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
  literal. `htons()`/`htonl()`
  are still right for `struct sockaddr_in`. Don't copy
  `src/p4/p4_devcmd.c`, which predates this.
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
memory. The TCP backend ([`rdma_backend_tcp.c`](src/rdma/rdma_backend_tcp.c)
and the `tcp_*.c` files) adds accept, health-check, and per-connection
receive threads (`qemu_thread_create()`).

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
([how to run it](#clang-tidy-and-the-thread-safety-analysis)).

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

- **Run [TSan](#sanitizers)**, and don't trust a pass: not every script
  fails on a server-side report, so grep
  `Testing/Temporary/LastTest.log` for `WARNING: ThreadSanitizer`.
- **A flaky test is a suspected race.** Find the race; don't retry,
  lengthen the timeout, or add a sleep.
- **A test must not race the threads it tests.** A thread acting at
  times the test can't observe (a startup pass, a periodic sweep) can
  disturb its setup: stop it first, set up before starting it, or wait
  on something it exposes, such as a counter. Set up only states the code
  can reach (e.g. not a mesh peer connecting to a lower node id, which
  the protocol forbids).
- **Loop new threaded tests so the timing moves**: hundreds of runs in
  one process, pinned with `taskset -c 0` (one race failed 13/300
  unpinned, 168/300 pinned). To confirm a window, add a delay there in a
  scratch copy and check the test fails.

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
  `_Static_assert` on their size.
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
  `popen()`): run a tool such as valgrind
  from outside (`--trace-children=yes`), and compile in built programs'
  paths with `$<TARGET_FILE:...>`, like `ERNIC_SERVER_PATH` in
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

**Every change must pass every check below for the files it touches; a
change with an outstanding finding isn't finished.** CI runs most of them
on every PR; locally, run [what's likely to fail](#what-to-run-locally)
from the repository root.

Fix the code rather than suppressing a finding. Suppress only the
unavoidable (vendored code, framework quirks), as narrowly as possible,
with a comment saying why.

### What to run locally

- **Always, for a C change**:
  - clang-format on the files you touched.
  - A GCC Debug build with `-DERNIC_WERROR=ON`, and all its tests.
  - GCC Release and Clang builds with `-DERNIC_WERROR=ON` (building is
    enough), preferably with CI's compilers, Ubuntu 24.04's GCC 13 and
    Clang 18, which warn about different things than newer ones.
  - clang-tidy, in a Debug build.
- **When the change calls for it**, with one compiler, running only the
  affected tests (`ctest -R`):
  - Shared state or threads: TSan, then the
    [loop](#testing-threaded-code) for new or changed threaded tests.
    Races are our most common CI failure; don't skip this. For a lock, a
    guarded field, or an annotation, also the clang 23 build.
  - Pointers, buffers, lengths, or outside input: ASan.
  - Wire data, or anything passed to `send()`/`write()`: valgrind.
  - New error paths or a lot of new code: coverage, if `src/` is near
    the floor.
  - Changed `#include`s: IWYU.
  - A new test: the revert check in [Review](#review-your-change).
- **Run what CI doesn't**: vermin, and any check whose workflow's
  `paths:` filter misses your change. The shell, CMake, Python,
  spelling, docs, and Ansible checks are quick; run each on the files of
  its kind you touch.
- **Leave to CI** the other compiler and build type for each sanitizer,
  clang-tidy in Release, valgrind on every test and server test, and
  coverage otherwise. When CI fails one, reproduce it locally.

### Formatting and warnings

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

### Sanitizers

- `-DERNIC_USE_SANITIZERS=ON` (ASan, LSan, UBSan, and compatible checks)
  and `-DERNIC_USE_THREAD_SANITIZER=ON` don't combine, so each needs its
  own build directory. Sanitizer flags go only in
  [`ErnicSanitizers.cmake`](cmake/ErnicSanitizers.cmake).
- **Every report is fatal.** The default run-time options are in
  [`sanitizer-default-options.c`](cmake/sanitizer-default-options.c);
  `*SAN_OPTIONS` override them. Run ASan's tests with
  `export ASAN_OPTIONS=detect_leaks=1`.
- **TSan**: build Debug with `-DCMAKE_C_FLAGS_DEBUG="-g -Og"`, as CI
  does, since `-O0` hides races. Run the tests under
  `setarch "$(uname -m)" -R`, as CI does; otherwise most abort with
  `FATAL: ThreadSanitizer: unexpected memory mapping` (ASLR colliding with
  shadow memory, not a test failure).
- **Under full ASan, the tests that start the server can time out**
  (`nvmeof-ci`, `s3-ci`, `ionic-ci`, `ionic-server-smoke`,
  `pci-config-test`). Only a real failure, or a timeout without
  sanitizers, counts against a change.

### Valgrind

Memcheck finds uninitialized reads the sanitizers miss, including bytes
passed to `send()`/`write()`. Build with **GCC**, without sanitizers,
with `-DERNIC_TRIVIAL_AUTO_VAR_INIT=OFF` (the default pattern-fill of
locals hides those reads), and with valgrind installed first so
`tcp-thread-stop-unit` finds `<valgrind/valgrind.h>` (it skips a
`RLIMIT_NOFILE` row valgrind breaks). CI runs every test
this way; locally, swap the `-E` for an `-R` naming the affected unit
tests:

```bash
ctest --test-dir build -T memcheck --output-on-failure \
  --overwrite "MemoryCheckCommand=$(command -v valgrind)" \
  --overwrite "MemoryCheckCommandOptions=--leak-check=full --track-origins=yes --num-callers=40 --error-exitcode=1" \
  -E '^(pci-config-test|find-rdma-device-tuple|find-ipv4-gid-index|server-lifecycle|ionic-server-smoke|ionic-ci|tcp-mesh-ci|nvmeof-ci|s3-ci|ionic-stats-interval-cli|ionic-eth-queues-cli|cmake-subproject|ernic-package-deb|ernic-package-deb-config-paths|ernic-vfu-client|ernic-ionic-driver|ernic-p4-driver|ernic-ionic-hw-stats|ernic-mesh-stress|ernic-mesh-flow|ernic-feature-macros)$'
```

The excluded tests run scripts, CMake, Python, or a server launcher.
Reports go to `build/Testing/Temporary/MemoryChecker.*.log`.

The server tests run the server under `ERNIC_TEST_SERVER_WRAPPER` (split
on whitespace); a new script that starts the server must too.
`test_server_lifecycle` can't take a command from the environment (see
the [scanner patterns](#patterns-the-security-scanners-report)), so run
it under valgrind with `--trace-children=yes`. Not every script checks
the server's exit, so check the logs. Limit the `-R` to the tests that
exercise the change:

```bash
rm -rf build/valgrind-server
mkdir build/valgrind-server
opts="--log-file=$PWD/build/valgrind-server/%p.log --leak-check=full --track-origins=yes --num-callers=40 --error-exitcode=1"
ERNIC_TEST_SERVER_WRAPPER="valgrind $opts" \
  ctest --test-dir build --output-on-failure \
  -R '^(pci-config-test|ionic-server-smoke|ionic-ci|tcp-mesh-ci|nvmeof-ci|s3-ci|ernic-vfu-client|ernic-ionic-driver|ernic-p4-driver|ernic-ionic-hw-stats|ernic-mesh-flow|ionic-stats-interval-cli|ionic-eth-queues-cli)$'
valgrind --trace-children=yes $opts build/tests/test_server_lifecycle
grep -L 'ERROR SUMMARY: 0 errors' build/valgrind-server/*.log
```

Valgrind doesn't check writes into mapped guest memory; the `memset`
rule in [Security](#security) covers those.

### clang-tidy and the thread-safety analysis

**clang-tidy** with [`.clang-tidy`](.clang-tidy): every finding is an
error. It needs clang-tidy 19+ and the same-version Clang. It runs during
the build, so a rebuild checks only what changed. CI
([clang-tidy.yml](.github/workflows/clang-tidy.yml)) uses clang-tidy 23,
Debug and Release (`NDEBUG` and `_FORTIFY_SOURCE` change the analysis);
locally, Debug is enough:

```bash
cmake -B build-tidy -G Ninja -DCMAKE_C_COMPILER=clang-23 \
  -DCMAKE_BUILD_TYPE=Debug -DERNIC_BUILD_P4=ON -DERNIC_USE_CLANG_TIDY=ON \
  -DERNIC_CLANG_TIDY_EXE=/usr/bin/clang-tidy-23
cmake --build build-tidy -- -k 0
```

`.clang-tidy` enables whole check groups, then disables, marked `TODO`,
checks the code doesn't pass yet. Never add to that list to silence a
finding; when the code passes a `TODO` check, delete its line and its
aliases'. Unavoidable findings get `// NOLINTNEXTLINE(check-name)` with a
reason.

**The [thread-safety analysis](#clangs-thread-safety-analysis)**: any
clang build, CI's clang 18 included, fails on a broken function
annotation. Fields and lock order need clang 23, which
[thread-safety.yml](.github/workflows/thread-safety.yml) uses, failing
only on these warnings:

```bash
cmake -B build-clang23 -G Ninja -DCMAKE_C_COMPILER=clang-23 \
  -DCMAKE_BUILD_TYPE=Debug -DERNIC_BUILD_P4=ON \
  -DCMAKE_C_FLAGS="-Werror=thread-safety -Werror=thread-safety-beta -Werror=thread-safety-negative -Werror=thread-safety-pointer"
cmake --build build-clang23 -- -k 0
```

### include-what-you-use

Use `apt install iwyu` with the clang it's built on
(`include-what-you-use --version`), in its own build directory (it
rejects options it doesn't know, such as GCC 15's
`-fzero-init-padding-bits`). A rebuild checks only what changed. To check
every file as CI ([iwyu.yml](.github/workflows/iwyu.yml)) does, start
from empty `build-iwyu` and `iwyu-runs` directories:

```bash
cmake -B build-iwyu -G Ninja -DCMAKE_C_COMPILER=clang-19 \
  -DCMAKE_BUILD_TYPE=Debug -DERNIC_BUILD_P4=ON -DERNIC_USE_IWYU=ON \
  -DERNIC_IWYU_EXE="$PWD/ci/iwyu/iwyu-wrapper.sh"
mkdir build-iwyu/iwyu-runs
ERNIC_IWYU_LOG_DIR="$PWD/build-iwyu/iwyu-runs" cmake --build build-iwyu
ci/iwyu/check-iwyu.sh build-iwyu/iwyu-runs
```

(Without `ERNIC_IWYU_EXE`, the suggestions are just in the build
output.) CI fails only if IWYU itself fails, and lists files with
suggestions. Apply the correct ones for files you touch (a header for a
symbol you get only indirectly; no `<stdint.h>` beside `<inttypes.h>`).
Ignore the wrong ones:

- Forward-declaring a module's opaque type (`struct nvmeof_cm;`) in a
  file that includes the module's header.
- Naming an internal header instead of the public one, or insisting on
  one of several headers that repeat a typedef (legal in C11, as
  `rocm_ernic_compat.h` and `rocm_ernic_internal.h` do for
  `rocm_ernic_dev_t`). Fix these with a mapping in
  [`cmake/iwyu-libc.imp`](cmake/iwyu-libc.imp) or
  [`cmake/iwyu-ernic.imp`](cmake/iwyu-ernic.imp), or an
  `// IWYU pragma: export` in the header that should provide the symbol.

### Static analysis

PRs labelled `run-static` also get clang-tidy, cppcheck, flawfinder, and
the Clang static analyzer, via `nix/analysis/`
([static-analysis.yml](.github/workflows/static-analysis.yml)). It never
fails, but read its findings for files you touched; its clang-tidy checks
aren't `.clang-tidy`'s.

### Shell

- **Shebangs**: `#!/usr/bin/env bash` for scripts run from the source
  tree, for systems like NixOS. Scripts installed onto a host or guest
  that may run as root (`service/`, the Ansible collection's `files/`)
  use `#!/bin/bash`, so they don't depend on `PATH`.
  `ansible/files/scripts/setup-ionic-dkms.sh` and
  `fetch-ionic-sources.sh` use
  `#!/bin/bash -eu`, despite their own `set -euo pipefail`, because
  `ansible-test sanity` accepts no other bash form; don't "tidy" it.
- **ShellCheck 0.11.0**, default checks only, over every `*.sh`,
  `*.sh.in`, and extension-less executable with a shell shebang (CI:
  [shellcheck.yml](.github/workflows/shellcheck.yml)):

  ```bash
  git ls-files | while IFS= read -r f; do
    if [[ "$f" == *.sh || "$f" == *.sh.in ]]; then
      echo "$f"
    elif [[ "${f##*/}" != *.* && -x "$f" ]] &&
      head -n1 -- "$f" | grep -Eqs '^#!.*\b(ba)?sh\b'; then
      echo "$f"
    fi
  done | xargs -d '\n' shellcheck
  ```

  Suppress only on the one command, with `# shellcheck disable=SCnnnn`
  and a reason (e.g. SC2329 for a cleanup function run only by `trap` in
  a script ending in `exit`).
- **[actionlint](https://github.com/rhysd/actionlint)**
  (`pip install actionlint-py`) checks workflows and ShellChecks their
  `run:` blocks; [`.github/actionlint.yaml`](.github/actionlint.yaml)
  ignores the unsupported `$/` form of `uses:`.
- **[`tools/check-embedded-shell.py`](tools/check-embedded-shell.py)**
  (needs PyYAML and ShellCheck 0.11.0; `--list` shows what it found)
  checks shell the others miss: Ansible `shell:`/`raw:` tasks (as `sh`
  unless `executable` is set), composite actions' `run:` steps, script
  heredocs in any of these, and workflow `run:` blocks GitHub runs under
  `sh` (no `shell:` in a job with `container:`; keep those POSIX or set
  `shell: bash`). For a heredoc to be checked, run a shell visibly
  (`'bash -s'`, `bash`, or `ssh` with no remote command) and quote the
  delimiter (`<<'EOF'`) unless the local shell must expand something.
  Prefer a heredoc to a quoted `ssh` command string, which isn't checked.

### CMake

**cmakelint** with [`.cmakelintrc`](.cmakelintrc) (CI:
[cmakelint.yml](.github/workflows/cmakelint.yml)):

```bash
git ls-files -z -- 'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake' |
  xargs -0 cmakelint
```

### Python

Python must run on **3.10** (no `tomllib`, `except*`, `typing.Self`, ...).
Run **black**, **pylint**, and **vermin** over every `*.py` and
extension-less python script except `nix/analysis/triage/`. pylint needs
`prometheus/requirements-exporter.txt` and PyYAML installed. CI
([pylint.yml](.github/workflows/pylint.yml)) runs black and pylint under
3.12, which miss most newer-Python usage, but not vermin
(`pip install vermin`), which prints `Target versions not met` on a
violation.

```bash
git ls-files -- ':!:nix/analysis/triage/' |
  while IFS= read -r f; do
    if [[ "$f" == *.py ]]; then
      echo "$f"
    elif [[ "${f##*/}" != *.* ]] &&
      head -n1 -- "$f" | grep -Eqs '^#!.*python'; then
      echo "$f"
    fi
  done > /tmp/python-files
xargs -d '\n' black --check < /tmp/python-files
xargs -d '\n' pylint < /tmp/python-files
xargs -d '\n' vermin -t=3.10- --violations --no-tips < /tmp/python-files
```

Suppress a pylint finding on the one line
(`# pylint: disable-next=...`) with a reason.

### Spelling, documentation, and Ansible

- **codespell** with [`.codespellrc`](.codespellrc) over the whole tree
  (CI: [codespell.yml](.github/workflows/codespell.yml)).
- **doc8**: `doc8 docs/ --max-line-length 80`.
- The docs must build without a Sphinx warning (needs Doxygen, not the C
  dependencies; CI: [docs-check.yml](.github/workflows/docs-check.yml)):

  ```bash
  cmake -B build-docs -DERNIC_DOCS_ONLY=ON -DERNIC_BUILD_DOCS=ON \
    -DERNIC_DOCS_WERROR=ON
  cmake --build build-docs --target sphinx-html
  ```

- **ansible-lint** (`--strict`) and **`ansible-test sanity`** on
  `ansible/`, with the commands, requirements, and ansible-lint version
  pinned in
  [ansible-collection.yml](.github/workflows/ansible-collection.yml);
  other versions report different problems.

## Forked and emulated code

**`src/pvrdma/`** is QEMU's PVRDMA device model core, forked and heavily
rewritten. Upstream deleted it, so it's ours to modify, built and checked
like the rest of `src/`. Keep each file's original copyright lines.
History: [src/pvrdma/README.md](src/pvrdma/README.md).

**`src/p4/`** emulates real P4, so **do what the real P4 tools do**, not
what seems reasonable: p4c for what a program may contain, P4Runtime for
what a table entry may be (the update record is P4Runtime-shaped), and
bmv2 for run-time behaviour. Check before deciding: p4c's
`backends/bmv2/common/control.h` (at the version `scripts/fetch-p4c.sh`
pins), the P4Runtime spec's "Match Format" section, and bmv2's
`docs/JSON_format.md`. p4c and P4Runtime win over bmv2's looser spec
(bmv2 allows a `ternary` table with no ternary key; p4c never emits one,
so the loader refuses it).

- **Refuse what they refuse, with `-EINVAL`**: a value with bits past its
  LPM prefix or outside its mask, a priority where P4Runtime forbids one,
  a second LPM key. Check a delete like an insert.
- **Depart only where our representation differs**, and document it in
  `docs/p4.rst`: the record always carries every key field, so a "don't
  care" field is a zero prefix or mask where P4Runtime omits it.
- **Compute a program's facts at load**, not at lookup
  (`p4_table.lpm_key`, not key 0), and add a case to
  `tests/data/p4/reject_cases.json` for each new load refusal.

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

**Every change a deployer, user, or contributor could notice needs a
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

## Known defects and traps

### TCP mesh GID resolution: never default a peer

The TCP backend once resolved every destination GID to one node (guessing
a node id from a GID octet, then falling back to node 0 or 1), silently
sending traffic for nodes 2+ to the wrong node.

Node identity is now configured, not inferred. Each instance gets its
guest RoCE addresses in `ERNIC_TCP_GUEST_GIDS` (comma-separated,
`inet_pton` notation; IPv4 stored as `::ffff:a.b.c.d`, as
`adminq_parse_roce_hdr()` builds it) and, once it knows its node id,
advertises them as `TCP_MSG_NODE_GIDS`. The manager fans these out, and
peers keep a `GID -> node id` table keyed on all 16 bytes.

`tcp_resolve_node_from_gid()` ([`tcp_gid.c`](src/rdma/tcp_gid.c)) is
only an exact lookup. A miss returns `UINT32_MAX`, and MODIFY_QP fails,
logging the full GID (`tcp_qp_state_rtr()` returns `-EINVAL`,
`rdma_rm_modify_qp()` `-EIO`). **Never reintroduce a default peer**: a
wrong node is a silent write into another guest's memory.

`service/rocm-ernic-launcher` derives each instance's IPv6 link-local GID
from its MAC, as the guest does; override it with `ERNIC_GUEST_GIDS_<n>`
when peers target a static address. Unlisted addresses are unreachable.
Reproducing a failure needs three or more nodes and the `-T` TAP plus
bridge setup. Covered by `tests/test_tcp_gid_resolve.c`.

### Dead and restarted mesh nodes

A dead node stays in `mesh_nodes` with `is_alive = false`. A node that
went quiet comes back under its id (a worker that loses the manager
registers again asking for it, `tcp_register_again()`), but a restarted
one registers again and gets a new id (ids are monotonic, so not stable
across runs). Both ids then claim the same GIDs.

So `TcpNodeGids` has an `alive` flag, cleared when the manager declares a
node dead and set when it answers or registers again, and
`tcp_gid_index_rebuild()` skips non-alive claimants: the restarted guest
owns its addresses under its new id, and a late node regains them
without re-advertising. Two *live* claimants still resolve to nothing.

- On the manager the flag copies `is_alive`: **change either only
  through `tcp_node_set_alive()`**, which writes both under
  `mesh_table_lock`. New claims take the node's `is_alive`
  (`tcp_gid_claims_set_if_member()`).
- Don't make the index read `is_alive` from `mesh_nodes`: that takes
  `mesh_table_lock` under `gid_table_lock`, against the lock order.
- A registration declares dead every node with no live connection
  (`tcp_declare_dead_if_unreachable()`), as the next health check would,
  **before it answers** (`tcp_register_declare_stale()`), because the
  answer lets a restarted worker claim the GIDs its old id still holds.
  Only the broadcasts wait for the answer.
- Workers keep no liveness of their own: the manager advertises a dead
  node as owning nothing (`tcp_snapshot_node_gids()`), which is what an
  empty advertisement means.

**Broadcasts** all come from one manager thread,
`tcp_broadcast_thread()`; handlers only ask for them
(`tcp_broadcast_topology()`, `tcp_broadcast_node()`,
`tcp_broadcast_node_gids()`, `tcp_broadcast_gid_table_to()`). A pass
sends a changed node's set to every worker, and the whole GID table only
to a node that just registered. **Don't send a topology or GID set from
anywhere else**: pruning against the topology (`tcp_gid_claims_prune()`)
is safe only because one thread sends the snapshots in the order it took
them. Ask only once a registration's answer has gone, since a worker
ignores what comes before it.

**Eviction.** Broadcasts carry at most `TCP_MAX_MESH_NODES` (64) nodes,
so when a node registers in a full table, the manager evicts the node
dead longest with no live connection (`tcp_evict_dead_node()`), drops its
claims, and advertises it as owning nothing; if none qualifies, it
refuses the newcomer with `-ENOSPC`. An evicted id is never reused, and
nothing is evicted once teardown has begun (`priv->stopping`, which
`tcp_register_again()` also checks).

- **Never keep a `MeshNodeInfo *` after dropping `mesh_table_lock`**:
  eviction frees it. Keep the node id and look it up again, as
  `TcpHealthCheckItem` does.
- A receive thread can outlive its connection's place in the table, so
  the manager records an advertisement only while its node is in
  `mesh_nodes`, checked under that lock
  (`tcp_gid_claims_set_if_member()`); otherwise a late one would restore
  an evicted node's claims.
- A worker cut off from the manager misses the eviction's empty
  advertisement, and later GID tables don't mention the node, so workers
  drop the claims of every node a topology update from the manager
  leaves out (`tcp_gid_claims_prune()`).

Covered by `test_dead_node_stops_owning_its_gids()` in
`tests/test_tcp_gid_resolve.c`, the eviction and liveness rows in
`tests/test_tcp_thread_stop.c`, and the churn rounds in
`tests/py/test_mesh_stress.py`.

### Connection lifetime

- A manager reaps connections that never registered
  (`tcp_reap_pending_conns()`) and retires a registered node's connection
  when it hangs up or stops answering (`tcp_health_check_retire()`). It
  never connects to a node; a worker reconnects to it
  (`tcp_worker_manager_pass()`).
- A worker closes a dead connection to another worker on its
  peer-connect thread's next pass (`tcp_worker_peer_sweep()`), and they
  reconnect. The accept thread and `tcp_worker_publish_peer()` replace a
  dead connection, never a live one.
- `TcpConnection` is reference counted, so the data path may hold one
  after it leaves its table; see its comment for the rules.
- A failed `accept()` (e.g. `EMFILE`, which leaves the connection queued)
  is retried after 100 ms. Without that backoff it once logged 295 GB.
- Registration hits a wall at about 40 nodes (measured on 64). The
  full-mesh connection cost behind it is inferred, **not** confirmed in
  the code.

### The mesh has no Ethernet or DHCP path

Guest TCP/IP uses the host TAP/bridge
([`ionic_eth_net.c`](src/ionic/ionic_eth_net.c)); the mesh carries only
`TCP_MSG_IONIC` and control traffic. Don't rebuild guest networking
inside it. The removed Ethernet and DHCP relay paths' ordinals remain as
`TCP_MSG_RESERVED_18..20`: renumbering would change the wire format,
bumping `TCP_PROTOCOL_VERSION` (checked at registration and the
worker-to-worker handshake) and cutting off every unupgraded peer.
