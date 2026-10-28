# AGENTS.md

Guidance for AI coding agents working in the rocm-ernic repository.
rocm-ernic is a userspace emulated RDMA NIC for virtual machines, built on
libvfio-user; see [README.md](README.md) for scope,
[docs/building.rst](docs/building.rst) for dependencies and CMake options,
and [docs/testing.rst](docs/testing.rst) for the test programs.

## Project direction

rocm-ernic will move into the
[rocm-systems](https://github.com/ROCm/rocm-systems) monorepo and be built
by [TheRock](https://github.com/ROCm/TheRock). Make changes that survive
that move:

- **Don't change link options without asking**: linker flags
  (`ernic_set_linker_flags()`, `target_link_options()`), `-pthread`, or
  whether something links statically or shared. TheRock is fussy about
  them, and a change that builds and tests fine here can break it. Other
  CMake changes are fine, within the rules below.
- **Keep the defaults safe for a superproject**, which has requirements a
  standalone build doesn't show. For example, `ERNIC_WERROR` is off by
  default so a compiler we haven't tried doesn't break a downstream build;
  CI turns it on.
- **Don't assume rocm-ernic is the top-level project.** New CMake code
  uses `PROJECT_*_DIR` or `CMAKE_CURRENT_*_DIR`, not
  `CMAKE_SOURCE_DIR`/`CMAKE_BINARY_DIR` (which older code, especially
  `tests/CMakeLists.txt`, still uses).
- **Namespace what you add**: `ERNIC_` for cache variables, `ernic-` for
  targets. Don't add more generic names like `doxygen`, `sphinx-html`, or
  `test_*`.
- **Set flags per target** with `ernic_set_compiler_flags()` and
  `ernic_add_sanitizers()` ([`cmake/`](cmake/)), never globally with
  `add_compile_options()` or `CMAKE_C_FLAGS`.

### Sister projects

- **hipObject** ([ROCm/hipObject](https://github.com/ROCm/hipObject)), an
  RDMA-accelerated S3 client for AMD GPUs. Its emulated-hardware CI runs
  on rocm-ernic's ionic NIC, and our S3 target speaks its protocol
  ([docs/s3.rst](docs/s3.rst)).
- **hipFile**, GPU-direct file I/O, in rocm-systems
  ([`projects/hipfile`](https://github.com/ROCm/rocm-systems/tree/develop/projects/hipfile)).
  It already builds with TheRock, so it's the model for rocm-ernic after
  the move; `cmake/ErnicSanitizers.cmake` follows its pattern.

Borrow ideas freely in either direction. If you find something one of
them should adopt, or a bug in them, ask whether to create a to-do file;
don't create one or edit the other project without asking.

## Configure and build

libvfio-user is built from source
([docs/building.rst](docs/building.rst); CI pins its commit in
[build-and-test.yml](.github/workflows/build-and-test.yml)).
Pass a non-standard prefix with `-DCMAKE_PREFIX_PATH`. Match CI:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DERNIC_WERROR=ON \
  -DERNIC_BUILD_P4=ON
cmake --build build
```

- The default build type is RelWithDebInfo, which CI doesn't build, so
  name the build type when matching CI. Without `ERNIC_WERROR`, warnings
  don't fail the build.
- `ERNIC_BUILD_P4` (off by default; needs json-c) builds `src/p4/` and
  its tests. CI's builds for the tests, clang-tidy, IWYU, and CodeQL turn
  it on, so a local build for any of them needs it too.
- CI builds GCC and Clang × Debug and Release × no sanitizer, ASan, and
  TSan, plus a Clang coverage build; the GCC Debug build also runs the
  tests under [valgrind](#c). **Build Release too**: it's the only CI
  configuration with GCC's dataflow warnings, `_FORTIFY_SOURCE=3`, and
  `NDEBUG`.
- Docs build without the C dependencies:
  `-DERNIC_DOCS_ONLY=ON -DERNIC_BUILD_DOCS=ON`.

## Tests

- Run `ctest --test-dir build --output-on-failure`. CI also runs
  `tests/test_loopback_ci.sh` and `tests/test_data_transfer`; see the
  workflow for their environment.
- **Every registered test runs without a VM, RDMA hardware, or root**, so
  a skipped test means something is wrong. Multi-VM and hardware testing
  ([docs/testing.rst](docs/testing.rst)) runs in the `system-tests`
  workflow and the `nvmeof-lane`, `s3-lane`, and `p4-lane` workflows it
  calls.
- A plain build sanitizes nothing, unit tests included; CI also builds
  with `ERNIC_USE_SANITIZERS` and `ERNIC_USE_THREAD_SANITIZER` (for when
  to run them locally, see
  [What to run locally](#what-to-run-locally)). Pass new tests to
  `ernic_set_compiler_flags()`; don't give them `-fsanitize` flags.
- **Run TSan tests under `setarch "$(uname -m)" -R`**, as CI does.
  Without it, most tests abort with `FATAL: ThreadSanitizer: unexpected
  memory mapping` (ASLR colliding with shadow memory, not a test failure).
- **Under full ASan, the tests that start the server can time out**:
  `nvmeof-ci`, `s3-ci`, `ionic-ci`, `ionic-server-smoke`,
  `pci-config-test`. That's not evidence against a change; a real failure,
  or a timeout without sanitizers, is.
- **Coverage of `src/` must not fall below `MIN_LINE_COVERAGE`** in
  [build-and-test.yml](.github/workflows/build-and-test.yml). Add tests
  for new or changed code, error paths included. CI's coverage leg
  measures it on every PR. To measure locally: Clang,
  `-DCMAKE_BUILD_TYPE=Debug` (as CI, so the lines match the source),
  `-DERNIC_USE_CODE_COVERAGE=ON`, `ctest`, then build `ernic-coverage`,
  which writes `coverage/report.txt` and `coverage/lines.txt`. Profiles
  accumulate; delete `coverage/profraw` to start over.

## C code

- Standard C11; no GNU extensions without permission. Atomics
  (`<stdatomic.h>`) and threads (`qemu/thread.h` over pthreads) are
  available; see [Thread safety](#thread-safety).
- **The library is POSIX.1-2008 plus glibc's `_DEFAULT_SOURCE`**, which
  `ernic_set_compiler_flags()` defines for every target. Don't add
  `_GNU_SOURCE` to a target or to `CMAKE_C_FLAGS`: it exposes every GNU
  function, and silences clang-tidy's `android-cloexec-*` checks. A file
  that needs a GNU or Linux extension (`accept4()`, `pipe2()`, `O_PATH`)
  writes `#define _GNU_SOURCE` before its first `#include`, with a comment
  naming what for, as `src/rdma/tcp_conn.c` does. For `environ`, declare
  `extern char **environ;` as POSIX requires, rather than defining
  `_GNU_SOURCE` for it.
- **Don't `goto` past an initialized declaration into its scope**
  (GCC's `-Wjump-misses-init`), as a `goto out` to the cleanup at the
  end of a function does over everything declared after it. Declare
  the variable before the first such `goto` and assign it where the
  initializer was (a struct with a compound literal), or make a
  constant table `static const`.
- **Write `/**` comments in Doxygen's syntax**: `@param name` and
  `@p name`, not kernel-doc's `@name:` and `@name`, which clang's
  `-Wdocumentation-pedantic` reports as unknown commands. Clang checks
  that each `@param` names a real parameter. A plain `/*` comment isn't
  checked.
- **Remove a parameter nothing needs.** Where a function pointer's type
  requires one the function doesn't use (a backend's ops, a library
  callback), write `(void)name;` in the function: `-Wunused-parameter`
  and clang-tidy's `misc-unused-parameters` both report it otherwise.
- **Include a project header by its path from `src/`**
  (`"pvrdma/rdma_rm.h"`, `"p4/p4_engine.h"`, `"byteorder.h"`), even from
  a file in the same directory: IWYU spells a header one way, so a bare
  `"rdma_rm.h"` beside `"pvrdma/rdma_rm.h"` looks like a missing include.
  The only other include roots are `shared/` and
  `src/qemu-compat/include/`, which stands in for QEMU's own headers
  (`"qemu/thread.h"`). Don't add a subdirectory of `src/` to a target's
  include path.
- **Use explicitly sized types**: `uint32_t`, `uint64_t`, `size_t` for
  sizes, never `long` for a wire or 64-bit value. Write constants with
  `UINT64_C()`, not `L`/`UL`.
- **Print fixed-width types with `<inttypes.h>` macros**: `"%" PRIu64`,
  `"%#" PRIx64`, `PRId64`, `PRIx32`; `%zu` for `size_t`. Never
  `%lu`/`%lx`/`%llu` for a `uint64_t`, and don't cast it to `unsigned
  long` or `unsigned long long` to fit one: `uint64_t` is `unsigned long`
  on x86-64 Linux, so an uncast `%lx` compiles cleanly here but is wrong
  where it isn't, and a cast just hides the type. `%ld` is right only for
  a real `long` (e.g. `ftell()`). A signed offset such as `loff_t` is cast
  to `uint64_t` only where it's known to be non-negative.
- **Parse numbers only with [`src/parse_int.h`](src/parse_int.h)**
  (`ernic_parse_int()`, `_u64()`, `_size()`, `_port()`; `_u64_span()`
  for a number inside a longer string), never `ato*`, `strto*`, or
  `sscanf()`, which miss errors or trailing text. Add helpers for new
  types there, with tests in `tests/test_parse_int.c`. Guest-side test
  programs include it too, so a VM building one needs a copy.
- **Lay out wire formats as offsets, not structs.** Anything on the wire
  or in guest memory is a byte buffer: give each field an `_OFF` macro
  and the whole a `_LEN`, with a comment drawing the layout and each
  field's byte order (see `nvmeof.h`, `net/net_headers.h`), and read and
  write the fields only with [`src/byteorder.h`](src/byteorder.h)'s
  `ernic_get_*()`/`ernic_put_*()`. Never cast a struct pointer over the
  buffer, use `__attribute__((packed))` (a GNU extension), byte-swap a
  struct's fields in place with `htonl()` and friends, or use
  `<endian.h>`'s `htole32()` family. Code that works on a decoded header
  converts it with a `_put()`/`_get()` pair, the only code that touches
  its bytes (`tcp_msg_hdr_put()`, `ionic_dp_wire_hdr_put()`), and a test
  checks the bytes it writes against a literal. `htons()` and `htonl()`
  remain right for `struct sockaddr_in`'s fields, which the socket API
  defines that way. `src/p4/p4_devcmd.c` predates this: its commands are
  naturally aligned structs, `memcpy()`'d in and out and converted with
  `<endian.h>`; don't copy it.
- **Allocate the way the layer does.**
  - `src/pvrdma/`, `src/qemu-compat/`, and `src/rdma/` are built on GLib's
    containers, which abort when out of memory, so they allocate with
    GLib too (`g_new0()`, `g_strdup()`, `g_malloc()`) and don't check
    the result: a `NULL` check there is dead code.
  - The exception is a buffer whose size the guest or a peer chooses.
    Allocate it with `malloc()` and check the result, so that a large
    request fails on its own instead of aborting the server; a test can
    fail `malloc()` with `--wrap`, but not GLib's allocators. See
    `tcp_recv_message()` and `tcp_queued_msg_new()`, and for the tests,
    `ST_NO_MEMORY` in `tests/test_tcp_thread_stop.c` and
    `tests/s3_alloc_fail.c`.
  - Everywhere else, use `malloc()`, `calloc()`, `realloc()`, and
    `strdup()`, and check every result. Don't use GLib's allocators
    there.
  - Free with the allocator's partner: `free()` for C's, `g_free()` for
    GLib's, including what a GLib function returns (e.g.
    `g_key_file_get_string()`). Where a buffer passes between the two
    kinds of code, comment which allocator it's from, as `TcpQueuedMsg`
    and `tcp_recv_message()` do.

## Thread safety

**Races here already cause intermittent CI failures.** Thread safety is a
correctness requirement for any change to shared state.

The main thread runs the vfio-user context and poll loop, and only it may
DMA into guest memory. The TCP backend
([`rdma_backend_tcp.c`](src/rdma/rdma_backend_tcp.c) and the `tcp_*.c`
files beside it) adds accept, health-check, and per-connection receive
threads (`qemu_thread_create()`).

- **Anything written by one thread and read by another needs a lock
  (`QemuMutex`) or an atomic.** Otherwise it's a data race (UB): the
  compiler may hoist a stop-flag load out of a loop, and the join hangs.
- **`volatile` is not synchronization**, except `volatile sig_atomic_t`
  for a signal handler's flag.
- **Comment any memory order weaker than the default** (seq_cst), saying
  why it suffices, like `recv_thread_running` in
  `rdma_backend_tcp_priv.h`.
- **Document which lock protects each shared field**, next to the field,
  and the order when code takes more than one lock. The TCP backend gives
  its whole order in one table, `Locks` at the top of
  [`rdma_backend_tcp_priv.h`](src/rdma/rdma_backend_tcp_priv.h); put a new
  lock there, not in the field's comment, and among the declarations of
  `TcpBackendPrivate`'s locks, which repeat the order.
- **Annotate it for clang's thread-safety analysis**
  ([`qemu/clang-tsa.h`](src/qemu-compat/include/qemu/clang-tsa.h)), which
  checks it at compile time. Use `QemuMutex`, which the analysis knows is
  a lock, not `pthread_mutex_t`. Mark a field its lock guards
  `TSA_GUARDED_BY(lock)`, declaring the lock first; a function whose caller
  holds a lock `TSA_REQUIRES`, and one that takes a lock, or joins a thread
  that does, `TSA_EXCLUDES`; and a new lock's place in the order
  `TSA_ACQUIRED_AFTER`. Every clang build checks the functions'
  annotations, but in C only clang 23 checks the fields' and the order
  ([thread-safety.yml](.github/workflows/thread-safety.yml)); see
  [C](#c) to run it.
- **The analysis can't follow everything.** It ties a field to a lock in
  another struct only when the code reaches the lock through the same
  object (`conn->priv->lock`, not a `priv` it can't tell is the same). It
  doesn't follow a lock taken conditionally (`if (sr) lock`, later `if
  (sr) unlock`); pair them, as `ionic_dp_deliver_recv()` does. A GLib
  table's contents are opaque to it, so the pointer to the table is what's
  annotated. Where it can't follow correct code (setup before an object
  is shared, teardown after its threads are joined, a read that the field's
  comment explains is safe without the lock), wrap the smallest region in
  `ROCM_ERNIC_WARN_THREAD_SAFETY_OFF`/`_ON`, with a comment saying which,
  or put it in a helper such as `tcp_conn_sockfd_unlocked()`. A field it
  can't express keeps only its comment, like `MeshNodeInfo`'s.
- **Don't hold a lock across a blocking call** (socket I/O, sleep,
  `qemu_thread_join()`) or a call that may take the same lock.
- **Every thread must be stoppable and joined**: an atomic stop flag
  polled with a bounded timeout, and a stop that wakes it if it blocks
  (`tcp_stop_accept_thread()` shuts down its socket;
  `tcp_stop_health_check_thread()` signals its condition variable).
- **TSan**: `-DERNIC_USE_THREAD_SANITIZER=ON` (not combinable with
  `ERNIC_USE_SANITIZERS`); for Debug also
  `-DCMAKE_C_FLAGS_DEBUG="-g -Og"`, as CI does, since `-O0` hides races.
  Run under `setarch` (see [Tests](#tests)). CI runs Debug and Release;
  locally, Debug is enough.
- **A passing test doesn't prove TSan was quiet**: not every script fails
  on a server-side report. Grep `Testing/Temporary/LastTest.log` for
  `WARNING: ThreadSanitizer`.
- **TSan can't see the guest.** Guest-shared memory (rings, indices) is
  written by another process; choose atomics and fences by reasoning
  about the guest driver.
- **A flaky test is a suspected race.** Don't retry it, lengthen its
  timeout, or add a sleep; find the race.
- **A test must not race the threads it tests.** A thread that acts at
  times the test can't observe (a first pass at startup, a periodic
  sweep) can disturb the test's setup. Stop it first, set up before
  starting it, or wait on something it exposes, such as a counter. Set up
  only states the code can really reach (e.g. not a mesh peer connecting
  to a lower node id, which the protocol forbids).
- **Loop new threaded tests in a way that moves the timing**: hundreds of
  runs in one process, pinned with `taskset -c 0` (one race failed 13/300
  unpinned, 168/300 pinned). To confirm a window, add a delay there in a
  scratch copy and check the test fails.

## Security

**Security is critical.** The emulator is a guest/host boundary: a bug can
expose host or other guests' memory, or take down a shared server.
Security beats speed and convenience.

- **Treat everything from outside the process as untrusted**: guest BAR
  and doorbell writes, admin commands, descriptors, ring indices, and the
  addresses, lengths, keys, and QP numbers in them; TCP mesh peers; and
  NVMe-oF and S3 clients. Check lengths, indices, and ranges before use;
  reject malformed input rather than truncating, clamping, or guessing.
  Check every address, key, or length against what was registered.
- **Integer arithmetic must be safe.**
  - Check for wrap before it happens: `len > limit || offset > limit -
    len`, not `offset + len > limit`. `__builtin_*_overflow` is a GNU
    extension; ask first.
  - Check ranges before narrowing; don't cast just to silence warnings.
  - Avoid UB: signed overflow, bad shift counts, out-of-range conversions
    to signed types.
  - No sanitizer reports unsigned wrap-around (it's defined); that's on
    you.
- **Bound every buffer**: no `strcpy`, `sprintf`, `strcat`; use
  `snprintf` and check for truncation.
- **`memset` any wire data to zero before filling it in**: completions,
  admin responses, anything DMAed to the guest, and anything sent to a
  peer. `= {0}` needn't zero padding. Give new wire structs a
  `_Static_assert` on their size.
- **Keep the hardening**: `_FORTIFY_SOURCE`
  ([`ErnicFortifySource.cmake`](cmake/ErnicFortifySource.cmake)),
  `-fstack-protector-strong`, `-fstack-clash-protection`, and PIE, full
  RELRO, and non-executable stack from `ernic_set_linker_flags()`
  ([`ErnicCompilerOptions.cmake`](cmake/ErnicCompilerOptions.cmake)).
  Don't weaken or remove any of it, and don't change the link options
  without asking (see [Project direction](#project-direction)).
- **Fix security scan findings** from CodeQL
  ([codeql.yml](.github/workflows/codeql.yml)) and the PR scan
  ([pr-security-scan.yml](.github/workflows/pr-security-scan.yml)); don't
  introduce alerts, and ask before dismissing one.

### Patterns the security scanners report

Past findings here. Avoid them everywhere, tests included (they're
scanned too). Add new general patterns here when a scanner reports one.

- **World-writable files or sockets** (`cpp/world-writable-file-creation`:
  `chmod()`/`open()`/`creat()` with e.g. `0666`). Grant owner, and group
  only if needed. For something that mustn't be reachable even briefly,
  create it under a restrictive `umask()`; narrowing afterwards lets a
  client connect in between. See `create_socket()` in
  `rocm_ernic_server.c`.
- **Check-then-act on a path** (`cpp/toctou-race-condition`: `stat()`,
  `lstat()`, or `access()` then `chmod()`, `unlink()`, `open()`, etc.).
  Act on a descriptor, or just do the operation and check the result. For
  name-only calls like `unlink()`, open the directory with `O_PATH` (not
  `O_RDONLY`, which needs read permission) and use `fstatat()`/
  `unlinkat()`, as `ernic_socket_remove_stale()` in `ernic_socket.c` does.
- **Running a program named by `getenv()` or `argv`**
  (`cpp/uncontrolled-process-operation`, into `exec*()`, `system()`,
  `popen()`). Run a tool such as valgrind from outside
  (`--trace-children=yes`); compile in paths of built programs with
  `$<TARGET_FILE:...>`, like `ERNIC_SERVER_PATH` in
  `test_server_lifecycle.c`.
- **A stack address stored where it can outlive its frame**
  (`cpp/stack-address-escape`), even if never used after return. Allocate
  the target instead, as `test_server_lifecycle.c` does for the server's
  `environ`.
- **An allocation sized by a peer, bounded only through a pointer**
  (`cpp/uncontrolled-allocation-size`). The query takes an upper-bound
  check only on a plain variable, so `if (hdr->len > MAX)` before
  `malloc(hdr->len)` still reports. Copy the length to a local, check
  that, and allocate with it, as `tcp_recv_message()` does.
- **Including a `.c` file** (`cpp/include-non-header`, clang-tidy
  `bugprone-suspicious-include`). A unit test needing private symbols
  includes the module's `_priv.h` (e.g.
  `src/ionic/ionic_adminq_priv.h`) and lists the module's `.c` in
  `tests/CMakeLists.txt`. Only the module and its tests include that
  header; prefix its functions with the module name.
- **Ignoring the result of a function a test stubs out**
  (`cpp/useless-expression`). CodeQL resolves a call to every definition
  of its callee in any target the file is built into, so where a unit
  test links a no-op stub (`ionic_eth_emu_queue_rx_frame()` in
  `tests/test_ionic_cq_arm.c`), a bare call statement in `src/` has "no
  effect". Check the result, or if dropping it is right, cast the call
  to `(void)` with a comment saying why, as `dp_s3_tx_frame()` in
  `ionic_datapath_s3.c` does.
- **`${{ }}` in a workflow `run:` block** (zizmor `template-injection`)
  for any value not fixed in the workflow, even one read from a repo
  file: it's pasted in before the shell parses it. Map it to `env:` and
  use `"$NAME"`, as `driver-build.yml`'s fetch step does.
- **Standard-library XML parsing** (Bandit B314: entity expansion). Parse
  with `xml.parsers.expat` and refuse a DOCTYPE from a
  `StartDoctypeDeclHandler` that raises, so no declaration is read;
  build a tree, if you need one, by feeding expat's callbacks to
  `xml.etree.ElementTree.TreeBuilder`. Bandit also flags any `xml.etree`
  import (B405) and `subprocess` import (B404): mark the import
  `# nosec` with the one rule and a comment saying why it's safe.

## Linters, warnings, and sanitizers (mandatory)

**Every change must pass every check below for the files it touches; a
change with an outstanding finding isn't finished.** That doesn't mean
running all of them locally: CI runs most of them on every PR. Run
[what to run locally](#what-to-run-locally) before pushing, from the
repository root, and let CI do the rest.

Fix the code rather than suppressing a finding. Suppress only the
unavoidable (vendored code, framework quirks), as narrowly as possible,
with a comment saying why.

### What to run locally

CI builds GCC and Clang × Debug and Release × no sanitizer, ASan, and
TSan, plus coverage and valgrind legs (see [Configure and
build](#configure-and-build)), and runs clang-tidy and IWYU in their own
workflows. Repeating that locally takes hours and rarely finds what CI
won't. Locally, catch what's likely to fail, scoped to what the change
touches. The commands are in the sections below.

- **Keep your build directories.** Name them `build-<config>` in the
  repository root (git ignores them) and reuse them: an incremental
  build recompiles, and re-runs clang-tidy or IWYU on, only what
  changed. A fresh directory for each check rebuilds everything.
- **Always**, for a C change:
  - clang-format on the files you touched.
  - A GCC Debug build with `-DERNIC_WERROR=ON` and all its tests:
    `ctest --test-dir build-<config> -j"$(nproc)" --output-on-failure`.
  - A GCC Release build and a Clang build, both with
    `-DERNIC_WERROR=ON`; building is enough. Prefer CI's compilers
    (Ubuntu 24.04's GCC 13 and Clang 18) if installed: they warn about
    different things than newer ones.
  - clang-tidy, in a Debug build directory.
- **When the change calls for it**, one compiler each, running only the
  tests that exercise the change (`ctest --test-dir build-<config> -R`):
  - Shared state or threads: TSan (Clang Debug, `-g -Og`, under
    `setarch`), then the [loop](#thread-safety) for new or changed
    threaded tests. Races are this project's most common CI failure;
    don't skip this one. If the change touches a lock, a guarded field or
    a thread-safety annotation, also the clang 23 thread-safety build
    (see [C](#c)).
  - Pointers, buffers, lengths, or input from outside the process: ASan
    (`ERNIC_USE_SANITIZERS`).
  - Wire data, or anything passed to `send()`/`write()`: valgrind on the
    affected tests, in a GCC build for valgrind (see [C](#c)).
  - New error paths or a lot of new code: coverage, if `src/` is near the
    floor.
  - Changed `#include`s: IWYU.
  - A new test: the revert check in
    [Review your change](#review-your-change-before-you-finish).
- **Leave to CI** the other compiler and build type for each sanitizer,
  clang-tidy in Release, valgrind on every test and every server test,
  and coverage otherwise. When CI fails one, reproduce that
  configuration locally.
- **Run what CI doesn't**: vermin, and any check whose workflow's
  `paths:` filter misses your change. The shell, CMake, Python,
  spelling, docs, and Ansible checks are quick; run each for any file of
  its kind you touch.

### C

- **clang-format 18** with [`.clang-format`](.clang-format) (fix with
  `clang-format-18 -i`; CI:
  [clang-format.yml](.github/workflows/clang-format.yml)):

  ```bash
  git ls-files -z -- '*.c' '*.h' | xargs -0 clang-format-18 --dry-run --Werror
  ```

  Use exactly clang-format 18.1.3, CI's version.

- **No warnings with GCC or Clang, Debug or Release**, each in its own
  build directory with `-DERNIC_WERROR=ON`; CI builds all four, locally
  see [what to run](#what-to-run-locally). Warning lists:
  [`ErnicGNUCompilerOptions.cmake`](cmake/ErnicGNUCompilerOptions.cmake),
  [`ErnicClangCompilerOptions.cmake`](cmake/ErnicClangCompilerOptions.cmake).
- **Clang thread-safety analysis** (see [Thread safety](#thread-safety)):
  every clang build warns about a broken function annotation, so CI's
  clang 18 build fails on one. Only clang 23 checks fields and lock order;
  [thread-safety.yml](.github/workflows/thread-safety.yml) builds with it,
  failing only on these warnings. Locally, in its own build directory:

  ```bash
  cmake -B build-clang23 -G Ninja -DCMAKE_C_COMPILER=clang-23 \
    -DCMAKE_BUILD_TYPE=Debug -DERNIC_BUILD_P4=ON \
    -DCMAKE_C_FLAGS="-Werror=thread-safety -Werror=thread-safety-beta -Werror=thread-safety-negative -Werror=thread-safety-pointer"
  cmake --build build-clang23 -- -k 0
  ```

- **Suppress warnings only with the macros in
  [`shared/rocm-ernic-warnings.h`](shared/rocm-ernic-warnings.h)**, never
  raw `#pragma`s, `-Wno-…`, or warning-list edits, and only when nearly
  impossible to fix (e.g. outside code). New suppression: add a
  `ROCM_ERNIC_WARN_<NAME>_OFF`/`_ON` pair, add both to `StatementMacros`
  in `.clang-format`, and wrap the smallest region. When removing one,
  delete macros left unused.
- **Sanitizer-clean**, with GCC and with Clang, in one build directory
  with `-DERNIC_USE_SANITIZERS=ON` (ASan, LSan, UBSan, and compatible
  checks; see [`ErnicSanitizers.cmake`](cmake/ErnicSanitizers.cmake)) and
  another with `-DERNIC_USE_THREAD_SANITIZER=ON`. CI runs both with both
  compilers, Debug and Release; locally one compiler is enough. Run
  `ctest` with `export ASAN_OPTIONS=detect_leaks=1`. Every report is
  fatal. Default run-time options (including TSan's `halt_on_error=1`) are
  in [`sanitizer-default-options.c`](cmake/sanitizer-default-options.c);
  `*SAN_OPTIONS` override them. Sanitizer flags go only in
  `ErnicSanitizers.cmake`.
- **Valgrind-clean** (memcheck finds uninitialized reads, including
  uninitialized bytes passed to `send()`/`write()`, that sanitizers
  don't). Use a **GCC** build without sanitizers, in its own build
  directory, configured with `-DERNIC_TRIVIAL_AUTO_VAR_INIT=OFF`: by
  default Clang and GCC 12+ fill uninitialized locals
  (`-ftrivial-auto-var-init=pattern`), which hides those reads. Install
  valgrind before configuring so
  `<valgrind/valgrind.h>` is found (`tcp-thread-stop-unit` uses it to
  skip a `RLIMIT_NOFILE` row valgrind breaks). CI's GCC Debug leg runs
  every test this way; locally, replace the `-E` with an `-R` naming the
  affected unit tests:

  ```bash
  ctest --test-dir build -T memcheck --output-on-failure \
    --overwrite "MemoryCheckCommand=$(command -v valgrind)" \
    --overwrite "MemoryCheckCommandOptions=--leak-check=full --track-origins=yes --num-callers=40 --error-exitcode=1" \
    -E '^(pci-config-test|find-rdma-device-tuple|find-ipv4-gid-index|server-lifecycle|ionic-server-smoke|ionic-ci|tcp-mesh-ci|nvmeof-ci|s3-ci|ionic-stats-interval-cli|ionic-eth-queues-cli|cmake-subproject|ernic-package-deb|ernic-vfu-client|ernic-ionic-driver|ernic-p4-driver|ernic-ionic-hw-stats|ernic-mesh-stress|ernic-mesh-flow)$'
  ```

  The excluded tests run scripts, CMake, Python, or a server launcher.
  Reports go to `build/Testing/Temporary/MemoryChecker.*.log`, not
  ctest's output.

  CI then runs the server itself under valgrind; locally, limit the `-R`
  below to the server tests that exercise the change. Test scripts run
  the server under `ERNIC_TEST_SERVER_WRAPPER` (split on whitespace); new
  scripts that start the server must too. `test_server_lifecycle` can't
  read a command from the environment (see the scanner patterns), so it
  runs under valgrind with `--trace-children=yes`. Not every script
  checks the server's exit, so check per-process logs:

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

  Valgrind doesn't check writes into mapped guest memory; that relies on
  the `memset` rule in [Security](#security).
- **clang-tidy clean** with [`.clang-tidy`](.clang-tidy): every finding
  is an error. Needs clang-tidy 19+ and the same-version Clang (GCC's
  options are rejected), in its own build directory. CI checks Debug and
  Release (`NDEBUG` and `_FORTIFY_SOURCE` change the analysis); locally,
  Debug is enough. It runs as part of the build, so rebuilding an
  existing directory checks only what changed. `-k 0` reports every
  failing file. CI
  ([clang-tidy.yml](.github/workflows/clang-tidy.yml)) uses clang-tidy
  23:

  ```bash
  cmake -B build-tidy -G Ninja -DCMAKE_C_COMPILER=clang-23 \
    -DCMAKE_BUILD_TYPE=Debug -DERNIC_BUILD_P4=ON -DERNIC_USE_CLANG_TIDY=ON \
    -DERNIC_CLANG_TIDY_EXE=/usr/bin/clang-tidy-23
  cmake --build build-tidy -- -k 0
  ```

  `.clang-tidy` enables whole check groups (excluded groups are listed
  with reasons), then disables, marked `TODO`, checks the code doesn't
  pass yet. Never add to that list to silence a finding. When the code
  passes a `TODO` check, delete its line and its aliases' lines.
  Unavoidable findings get `// NOLINTNEXTLINE(check-name)` with a reason.
- **include-what-you-use** (`apt install iwyu`), with the clang IWYU is
  built on (`include-what-you-use --version`), in its own build
  directory; IWYU rejects options it doesn't know, such as GCC 15's
  `-fzero-init-padding-bits`. Run it locally when you change
  `#include`s; IWYU skips up-to-date files, so rebuilding an existing
  directory checks only what changed. To check every file as CI
  ([iwyu.yml](.github/workflows/iwyu.yml)) does, start from an empty
  build directory and `iwyu-runs`:

  ```bash
  cmake -B build-iwyu -G Ninja -DCMAKE_C_COMPILER=clang-19 \
    -DCMAKE_BUILD_TYPE=Debug -DERNIC_BUILD_P4=ON -DERNIC_USE_IWYU=ON \
    -DERNIC_IWYU_EXE="$PWD/ci/iwyu/iwyu-wrapper.sh"
  mkdir build-iwyu/iwyu-runs
  ERNIC_IWYU_LOG_DIR="$PWD/build-iwyu/iwyu-runs" cmake --build build-iwyu
  ci/iwyu/check-iwyu.sh build-iwyu/iwyu-runs
  ```

  (Omit `ERNIC_IWYU_EXE` to just see suggestions in the build output.)
  CI fails only if IWYU itself fails, and lists files with suggestions.
  Apply the correct suggestions for files you touch, such as adding a
  header for a symbol you only get indirectly, or dropping `<stdint.h>`
  where `<inttypes.h>` is included. Ignore wrong ones:
  - Forward-declaring a module's opaque type (e.g. `struct nvmeof_cm;`)
    in a file that includes the module's header.
  - Naming an internal header instead of the public one, or insisting on
    one of several headers that repeat a typedef (legal in C11;
    `rocm_ernic_compat.h` and `rocm_ernic_internal.h` both do for
    `rocm_ernic_dev_t`). Fix these with a mapping in
    [`cmake/iwyu-libc.imp`](cmake/iwyu-libc.imp) or
    [`cmake/iwyu-ernic.imp`](cmake/iwyu-ernic.imp), or an
    `// IWYU pragma: export` in the header that should provide the
    symbol.
- **Static analysis** (clang-tidy, cppcheck, flawfinder, Clang static
  analyzer, via `nix/analysis/`) runs on PRs labelled `run-static`
  ([static-analysis.yml](.github/workflows/static-analysis.yml)). It
  never fails, but read its findings for files you touched. Its
  clang-tidy checks differ from `.clang-tidy`, so it doesn't replace the
  check above.

### Shell

- **Shebangs**: `#!/usr/bin/env bash` for scripts run from the source
  tree (tests, `ci/`, most of `scripts/`), for systems like NixOS.
  Scripts installed onto a host or guest that may run as root
  (`service/`, the Ansible collection's `files/`, and the driver
  pack's `ansible/files/scripts/setup-ionic-dkms.sh` and
  `fetch-ionic-sources.sh`) keep an absolute `#!/bin/bash`, so they
  don't depend on `PATH`.
  Those two write it `#!/bin/bash -eu`, redundantly with their own
  `set -euo pipefail`: they ship inside the Ansible collection, and
  `ansible-test sanity`'s shebang check runs over everything in it and
  accepts only `#!/bin/sh`, `#!/bin/bash -eu`, `#!/bin/bash -eux` and
  the `#!/usr/bin/env` forms. Don't "tidy" the flags away.
- **ShellCheck 0.11.0**, default checks only (no
  `--list-optional`), over every `*.sh`, `*.sh.in`, and extension-less
  executable with a shell shebang (CI:
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

- **Suppress only on the one command**, with `# shellcheck
  disable=SCnnnn` and a reason. Known case: a cleanup function run only
  by `trap` in a script ending in `exit` (SC2329).
- **[actionlint](https://github.com/rhysd/actionlint)**
  (`pip install actionlint-py`; run `actionlint`) checks workflows and
  ShellChecks their `run:` blocks. Config in
  [`.github/actionlint.yaml`](.github/actionlint.yaml): an ignore for
  the unsupported `$/` form of `uses:`.
- **[`tools/check-embedded-shell.py`](tools/check-embedded-shell.py)**
  (needs PyYAML and ShellCheck 0.11.0) checks shell the others miss,
  reporting at the original file and line (`--list` shows what it
  found): Ansible `shell:`/`raw:` tasks (as `sh` unless `executable` is
  set), composite actions' `run:` steps, script heredocs in any of
  these, and workflow `run:` blocks GitHub runs under `sh` (a step
  without `shell:` in a job with `container:`; keep those POSIX or set
  `shell: bash`). For a heredoc to be checked, make the command visibly
  run a shell (`'bash -s'`, `bash`, or `ssh` with no remote command) and
  quote the delimiter (`<<'EOF'`) unless the local shell must expand
  something. A quoted command string passed to `ssh` isn't checked, so
  prefer a heredoc for anything longer than one command.

### CMake

- **cmakelint** with [`.cmakelintrc`](.cmakelintrc) (CI:
  [cmakelint.yml](.github/workflows/cmakelint.yml)):

  ```bash
  git ls-files -z -- 'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake' |
    xargs -0 cmakelint
  ```

### Python

Python must run on **3.10**: nothing newer, such as `tomllib`, `except*`,
or `typing.Self`.

- **black**, **pylint**, and **vermin** over every `*.py` and
  extension-less python-shebang script, except `nix/analysis/triage/` and
  `.cmake-format.py`. pylint needs `prometheus/requirements-exporter.txt`
  and PyYAML installed. vermin (`pip install vermin`) isn't in CI, and
  black and pylint (run under 3.12 in CI:
  [pylint.yml](.github/workflows/pylint.yml)) miss most newer-Python
  usage; it prints `Target versions not met` on a violation.

  ```bash
  git ls-files -- ':!:nix/analysis/triage/' ':!:.cmake-format.py' |
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

- **Suppress pylint findings on the one line**
  (`# pylint: disable-next=...`) with a reason.

### Spelling and documentation

- **codespell** with [`.codespellrc`](.codespellrc), whole tree:
  `codespell` (CI: [codespell.yml](.github/workflows/codespell.yml)).
- **doc8**: `doc8 docs/ --max-line-length 80`. The docs must build (needs
  Doxygen, not the C dependencies; CI:
  [docs-check.yml](.github/workflows/docs-check.yml)):

  ```bash
  cmake -B build-docs -DERNIC_DOCS_ONLY=ON -DERNIC_BUILD_DOCS=ON
  cmake --build build-docs --target sphinx-html
  ```

### Ansible

- **ansible-lint** (`--strict`, so warnings fail) and
  **`ansible-test sanity`** on `ansible/`; exact commands, requirements,
  and the pinned ansible-lint version are in
  [ansible-collection.yml](.github/workflows/ansible-collection.yml). Use
  that version; others report different problems.

## The QEMU code in `src/pvrdma/`

`src/pvrdma/` is the core of QEMU's PVRDMA device model, forked and
heavily rewritten. Upstream deleted it, so **it's ours to modify**, built
and checked like the rest of `src/`. Keep each file's original copyright
lines. History: [src/pvrdma/README.md](src/pvrdma/README.md).

## The P4 engine in `src/p4/`

The engine emulates real P4, so **do what the real P4 tools do**, not
what seems reasonable: p4c for what a program may contain, P4Runtime for
what a table entry may be (the update record is P4Runtime-shaped), and
bmv2 for run-time behaviour. Check their source or spec before deciding:
p4c's `backends/bmv2/common/control.h` (at the version
`scripts/fetch-p4c.sh` pins), the P4Runtime spec's "Match Format"
section, and bmv2's `docs/JSON_format.md`. Where they disagree, p4c and
P4Runtime win over bmv2's looser spec (e.g. bmv2 allows a `ternary` table
with no ternary key; p4c never emits one, and the loader refuses it).

- **Refuse what they refuse, with `-EINVAL`, rather than accepting it
  leniently**: a value with bits past its LPM prefix or outside its
  mask, a priority where P4Runtime forbids one, a second LPM key. Check a
  delete like an insert.
- **Depart only where our representation differs**, and document it
  (`docs/p4.rst`). The record always carries every key field, so a
  "don't care" field is a zero prefix or mask, where P4Runtime omits it.
- **A program's facts are computed at load**, not assumed at lookup
  (e.g. `p4_table.lpm_key`, not key 0). Add a case to
  `tests/data/p4/reject_cases.json` for each new load refusal.

## Copyright lines

AMD copyright lines carry no year. Don't add one to a new file or
"update" one on an existing file. Two forms, by license:

- **MIT** (`CMakeLists.txt`, `flake.nix`, `cmake/`, `docs/`, `ansible/`,
  `ci/`, `.github/`, `prometheus/`, `udev/`, `scripts/`, `service/`,
  `tools/`, `nix/` except `nix/analysis/fuzz/`, and `shared/` headers),
  matching `LICENSE.md`:
  `Copyright (c) Advanced Micro Devices, Inc. All rights reserved.`
- **GPL** (`src/`, `tests/`, `nix/analysis/fuzz/`):
  `Copyright (C) Advanced Micro Devices, Inc.` (capital `(C)`, no "All
  rights reserved"), followed by the QEMU GPL boilerplate.

Leave third-party copyright lines exactly as they are, years included.
Patches under `ansible/files/patches/` take the licenses of the Linux ionic and
rdma-core files they modify (see `LICENSE.md`); don't add a copyright or
license notice to them.

## Changelog

**Every change a deployer, user, or contributor could notice needs an
entry in [CHANGELOG.md](CHANGELOG.md) in the same change**: the
guest-visible device, server options and environment variables,
`ernicctl`, service and udev files, ernic-exporter metrics, the mesh
protocol, CMake options, installed files and packages, dependencies, and
notable CI and test additions. Purely internal refactoring needs none.

Add it under `UNRELEASED`, in `Added`, `Changed`, `Removed`, `Fixed`,
`Limitations`, or `Known issues`. Write for users, like the existing
entries: what changed and what they must do differently. Start
deployment-breaking entries with `**Breaking:**`.

The Ansible collection has its own changelog: add a fragment to
`ansible/changelogs/fragments/` using the sections in
`ansible/changelogs/config.yaml`.

## Review your change before you finish

**Before calling a change done, review the whole of `git diff` and any
new files as a reviewer would**, not just the lines you meant to change.
Every PR gets a Copilot review; these are what it has asked for most.
Fix what you find, then rerun the
[local checks](#what-to-run-locally).

- **Is there a changelog entry?** Its most common request. File
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
  initialized on that path? Is partial setup torn down and every
  descriptor closed? Setup failure in a test is a failure, not a skip
  (exit 77). Cover new error paths for the coverage floor.
- **Does a test wait for events, not time?** Wait with a deadline for
  something observable (a log line, a counter, a registration), not a
  fixed sleep.
- **Is every shared field read under its lock?** Including reads that
  build a message sent after unlocking.
- **Does it follow this file?** E.g. parsing through `parse_int.h` with a
  helper whose range fits, byte order matching the other end of the wire,
  `sh`-compatible `run:` blocks in container jobs.
- **Do the words match the code?** Comments, docs, changelog, and PR
  description claim only what the diff does: no check described as
  stronger than it is, no guarantee on one path described as holding on
  all.

## Lab provisioning

Set up guests and hosts with the Ansible collection; see
[ansible/README.md](ansible/README.md).

## Known defects and traps

### TCP mesh GID resolution: never default a peer

The TCP backend once resolved every destination GID to one node (by
guessing a node id from a GID octet, then falling back to a hardcoded
node 0 or 1), silently sending traffic for nodes 2+ to the wrong node.

Node identity is now configured, not inferred. Each instance gets its
guest RoCE addresses in `ERNIC_TCP_GUEST_GIDS` (comma-separated,
`inet_pton` notation; IPv4 is stored as `::ffff:a.b.c.d`, the form
`adminq_parse_roce_hdr()` in `src/ionic/ionic_adminq.c` builds). Once it
knows its node id it advertises them as `TCP_MSG_NODE_GIDS`; the manager
fans these out, and peers keep a `GID -> node id` table keyed on all 16
bytes.

`tcp_resolve_node_from_gid()` in [`tcp_gid.c`](src/rdma/tcp_gid.c) is
only an exact lookup. A miss returns `UINT32_MAX`; `tcp_qp_state_rtr()`
returns `-EINVAL`, `rdma_rm_modify_qp()` turns it into `-EIO`, and the
adminq MODIFY_QP path fails the command, logging the full GID. **Never
reintroduce a default peer**: a wrong node is a silent write into another
guest's memory.

`service/rocm-ernic-launcher` derives each instance's IPv6 link-local GID
from its MAC (what the guest autoconfigures); override with
`ERNIC_GUEST_GIDS_<n>` when peers target a static address. Unlisted
addresses are unreachable. Reproducing a failure needs three or more
nodes and the `-T` TAP plus bridge setup. Covered by
`tests/test_tcp_gid_resolve.c`.

### Dead and restarted mesh nodes

A dead node stays in `mesh_nodes`, marked `is_alive = false`. A node
that went quiet returns under its id (a worker that loses its connection
to the manager registers again asking for it, `tcp_register_again()`),
but a restarted one registers again and gets a new id (ids are
monotonic, so not stable across runs). Both ids then claim the same
GIDs.

So `TcpNodeGids` has an `alive` flag, which the manager clears when it
declares a node dead and sets when it answers or registers again, and
`tcp_gid_index_rebuild()` skips non-alive claimants: the returning guest
owns its addresses under its new id, and a late node regains them
without re-advertising. Two *live* claimants still resolve to nothing.
On the manager the flag is a copy of `is_alive`: **change either only
through `tcp_node_set_alive()`**, which writes both under
`mesh_table_lock`, and new claims take the node's `is_alive`
(`tcp_gid_claims_set_if_member()`). A registration declares dead every
node with no live connection (`tcp_declare_dead_if_unreachable()`), as
the health check would on its next pass, and **before it answers**
(`tcp_register_declare_stale()`): the answer lets a restarted worker
claim the GIDs its old id still holds. Only the broadcasts wait for the
answer.
Workers keep no liveness of their own; the manager advertises a dead node
as owning nothing (`tcp_snapshot_node_gids()`), which is what gives an
empty advertisement meaning.

The manager's broadcasts all come from one thread
(`tcp_broadcast_thread()`). Handlers only ask for them
(`tcp_broadcast_topology()`, `tcp_broadcast_node()`,
`tcp_broadcast_node_gids()`, `tcp_broadcast_gid_table_to()`), and a pass
sends a changed node's set to every worker, and the whole GID table only
to a node that has just registered. **Don't send a topology or a GID set
from anywhere else**: pruning against the topology
(`tcp_gid_claims_prune()`) is safe only because one thread sends the
snapshots in the order it took them. Ask only once the answer to a
registration has gone, since a worker ignores what comes before it.

The broadcasts carry at most `TCP_MAX_MESH_NODES` (64) nodes, so when a
new node registers in a full table, the manager evicts the node that has
been dead longest and has no live connection (`tcp_evict_dead_node()`),
drops its claims, and advertises it as owning nothing. If no node can be
evicted, it refuses the new one with `-ENOSPC`, so the table never holds
more than the broadcasts carry. An evicted id is never given out again.
**Never keep a `MeshNodeInfo *` after dropping `mesh_table_lock`**:
eviction frees it. Keep the node id and look it up again, as
`TcpHealthCheckItem` does. A receive thread can outlive its connection's
place in the table, so the manager records a worker's advertisement only
while its node is in `mesh_nodes`, checked under that lock
(`tcp_gid_claims_set_if_member()`); otherwise a late one would put an
evicted node's claims back. Nothing is evicted once teardown has begun
(`priv->stopping`), as with `tcp_register_again()`. A worker cut off from
the manager misses the eviction's empty advertisement, and later GID
tables don't mention the node, so a worker drops the claims of every node
a topology update from the manager leaves out
(`tcp_gid_claims_prune()`).

Don't make the index read `is_alive` from `mesh_nodes`: that takes
`mesh_table_lock` under `gid_table_lock`, the reverse of the lock order.
Covered by `test_dead_node_stops_owning_its_gids()` in
`tests/test_tcp_gid_resolve.c`, the eviction and liveness rows in
`tests/test_tcp_thread_stop.c`, and the churn rounds in
`tests/py/test_mesh_stress.py`.

### Connection lifetime

- A manager reaps connections that never registered
  (`tcp_reap_pending_conns()`) and retires a registered node's connection
  when it hangs up or stops answering (`tcp_health_check_retire()`). It
  never connects to a node; a worker owns its link to the manager and
  connects again itself (`tcp_worker_manager_pass()`). A worker closes a
  dead connection to another worker on its peer-connect thread's next
  pass (`tcp_worker_peer_sweep()`), and they reconnect; the accept thread
  and `tcp_worker_publish_peer()` replace a dead connection, never a live
  one. `TcpConnection` is reference counted, so the data path may hold
  one after it leaves its table; see its comment for the rules.
- A failed `accept()` (e.g. `EMFILE`, which leaves the connection queued)
  is retried after 100 ms. Without that backoff it once logged 295 GB.
- A registration wall appears at about 40 nodes (measured on 64). The
  full-mesh connection cost behind it is inferred, **not** confirmed in
  the code.

### Trap: the mesh has no Ethernet or DHCP path

Guest TCP/IP uses the host TAP/bridge
([`ionic_eth_net.c`](src/ionic/ionic_eth_net.c)). The mesh carries only
`TCP_MSG_IONIC` and control traffic. The old never-used Ethernet and
DHCP relay paths are gone, but their ordinals remain as
`TCP_MSG_RESERVED_18..20`. Renumbering the message types would change
the wire format, which means bumping `TCP_PROTOCOL_VERSION` (checked at
registration and at the worker-to-worker handshake) and cutting off every
unupgraded peer, all to reclaim three numbers. Don't rebuild guest
networking inside the mesh.
