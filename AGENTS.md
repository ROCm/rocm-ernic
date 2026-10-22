# AGENTS.md

Guidance for AI coding agents working in the rocm-ernic repository.
rocm-ernic is a userspace emulated RDMA NIC for virtual machines, built on
libvfio-user; see [README.md](README.md) for scope,
[INSTALL.md](INSTALL.md) for dependencies and CMake options, and
[docs/testing.rst](docs/testing.rst) for the test programs.

## Project direction

rocm-ernic will move into the
[rocm-systems](https://github.com/ROCm/rocm-systems) monorepo and be built
by [TheRock](https://github.com/ROCm/TheRock). Make changes that survive
that move:

- **Ask before changing compile or link options, dependency handling, or
  other non-trivial CMake.** A superproject has requirements a standalone
  build doesn't show. For example, `ERNIC_WERROR` is off by default so a
  compiler we haven't tried doesn't break a downstream build; CI turns it
  on.
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

libvfio-user is built from source ([INSTALL.md](INSTALL.md); CI pins its
commit in [build-and-test.yml](.github/workflows/build-and-test.yml)).
Pass a non-standard prefix with `-DCMAKE_PREFIX_PATH`. Match CI:

```bash
cmake -B build -G Ninja -DERNIC_WERROR=ON
cmake --build build
```

- The default build type is Debug. Without `ERNIC_WERROR`, warnings don't
  fail the build.
- CI builds GCC and Clang × Debug and Release × no sanitizer, ASan, and
  TSan, plus a Clang coverage build; the GCC Debug build also runs the
  tests under [valgrind](#c). **Build Release too**: it's the only
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
  ([docs/testing.rst](docs/testing.rst)) runs in the `system-tests`,
  `nvmeof-lane`, `s3-lane`, and `self-hosted-ci` workflows.
- A plain build sanitizes nothing, unit tests included; also build with
  `ERNIC_USE_SANITIZERS` and `ERNIC_USE_THREAD_SANITIZER`. Pass new tests
  to `ernic_set_compiler_flags()`; don't give them `-fsanitize` flags.
- **Run TSan tests under `setarch "$(uname -m)" -R`**, as CI does.
  Without it, most tests abort with `FATAL: ThreadSanitizer: unexpected
  memory mapping` (ASLR colliding with shadow memory, not a test failure).
- **Under full ASan, the tests that start the server can time out**:
  `nvmeof-ci`, `s3-ci`, `ionic-ci`, `ionic-server-smoke`,
  `pci-config-test`. That's not evidence against a change; a real failure,
  or a timeout without sanitizers, is.
- **Coverage of `src/` must not fall below `MIN_LINE_COVERAGE`** in
  [build-and-test.yml](.github/workflows/build-and-test.yml). Add tests
  for new or changed code, error paths included. To measure: Clang,
  `-DERNIC_USE_CODE_COVERAGE=ON`, `ctest`, then build `ernic-coverage`,
  which writes `coverage/report.txt` and `coverage/lines.txt`. Profiles
  accumulate; delete `coverage/profraw` to start over.

## C code

- Standard C11; no GNU extensions without permission. Atomics
  (`<stdatomic.h>`) and threads (`qemu/thread.h` over pthreads) are
  available; see [Thread safety](#thread-safety).
- **Use explicitly sized types**: `uint32_t`, `uint64_t`, `size_t` for
  sizes, never `long` for a wire or 64-bit value. Print with `PRIu64`,
  `PRIx32`, `%zu` (not `%lu`/`%ld`); write constants with `UINT64_C()`,
  not `L`/`UL`.
- **Parse numbers only with [`src/parse_int.h`](src/parse_int.h)**
  (`ernic_parse_int()`, `_u64()`, `_size()`, `_port()`; `_u64_span()`
  for a number inside a longer string), never `ato*`, `strto*`, or
  `sscanf()`, which miss errors or trailing text. Add helpers for new
  types there, with tests in `tests/test_parse_int.c`. Guest-side test
  programs include it too, so a VM building one needs a copy.

## Thread safety

**Races here already cause intermittent CI failures.** Thread safety is a
correctness requirement for any change to shared state.

The main thread runs the vfio-user context and poll loop, and only it may
DMA into guest memory. The TCP backend
([`rdma_backend_tcp.c`](src/rdma/rdma_backend_tcp.c)) adds accept,
health-check, and per-connection receive threads
(`qemu_thread_create()`).

- **Anything written by one thread and read by another needs a lock
  (`QemuMutex`) or an atomic.** Otherwise it's a data race (UB): the
  compiler may hoist a stop-flag load out of a loop, and the join hangs.
- **`volatile` is not synchronization**, except `volatile sig_atomic_t`
  for a signal handler's flag.
- **Comment any memory order weaker than the default** (seq_cst), saying
  why it suffices, like `recv_thread_running` in `rdma_backend_tcp.c`.
- **Document which lock protects each shared field**, next to the field,
  and the order when code takes more than one lock.
- **Don't hold a lock across a blocking call** (socket I/O, sleep,
  `qemu_thread_join()`) or a call that may take the same lock.
- **Every thread must be stoppable and joined**: an atomic stop flag
  polled with a bounded timeout, and a stop that wakes it if it blocks
  (`tcp_stop_accept_thread()` shuts down its socket;
  `tcp_stop_health_check_thread()` signals its condition variable).
- **TSan in Debug and Release**: `-DERNIC_USE_THREAD_SANITIZER=ON` (not
  combinable with `ERNIC_USE_SANITIZERS`); for Debug also
  `-DCMAKE_C_FLAGS_DEBUG="-g -Og"`, as CI does, since `-O0` hides races.
  Run under `setarch` (see [Tests](#tests)).
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
  Ask before changing any compile or link option.
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
  client connect in between. See the vfio-user socket in
  `rocm_ernic_server.c`.
- **Check-then-act on a path** (`cpp/toctou-race-condition`: `stat()`,
  `lstat()`, or `access()` then `chmod()`, `unlink()`, `open()`, etc.).
  Act on a descriptor, or just do the operation and check the result. For
  name-only calls like `unlink()`, open the directory with `O_PATH` (not
  `O_RDONLY`, which needs read permission) and use `fstatat()`/
  `unlinkat()`, as `remove_stale_socket()` in `rocm_ernic_server.c` does.
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
- **Including a `.c` file** (`cpp/include-non-header`, clang-tidy
  `bugprone-suspicious-include`). A unit test needing private symbols
  includes the module's `_priv.h` (e.g. `src/ionic_adminq_priv.h`) and
  lists the module's `.c` in `tests/CMakeLists.txt`. Only the module and
  its tests include that header; prefix its functions with the module
  name.
- **`${{ }}` in a workflow `run:` block** (zizmor `template-injection`)
  for any value not fixed in the workflow, even one read from a repo
  file: it's pasted in before the shell parses it. Map it to `env:` and
  use `"$NAME"`, as `driver-build.yml`'s fetch step does.
- **Standard-library XML parsing** (Bandit B314: entity expansion). Use
  expat and refuse a DOCTYPE, like `parse_xml()` in
  `ci/report/gen-report.py`. Bandit also flags any `xml.etree` import
  (B405) and `subprocess` import (B404): mark the import `# nosec` with
  the one rule and a comment saying why it's safe.

## Linters, warnings, and sanitizers (mandatory)

**Every change must pass every check below for the files it touches; a
change with an outstanding finding isn't finished.** Some CI workflows
are path-filtered (Build and Test doesn't run for `shared/`-only
changes) and some checks aren't in CI, so green CI isn't proof. Run them
yourself, from the repository root.

Fix the code rather than suppressing a finding. Suppress only the
unavoidable (vendored code, framework quirks), as narrowly as possible,
with a comment saying why.

### C

- **clang-format 18** with [`.clang-format`](.clang-format) (fix with
  `clang-format-18 -i`; CI:
  [clang-format.yml](.github/workflows/clang-format.yml)):

  ```bash
  git ls-files -z -- '*.c' '*.h' | xargs -0 clang-format-18 --dry-run --Werror
  ```

  Use exactly clang-format 18.1.3, CI's version.

- **No warnings with GCC or Clang, Debug or Release**, each in its own
  build directory with `-DERNIC_WERROR=ON`. Warning lists:
  [`ErnicGNUCompilerOptions.cmake`](cmake/ErnicGNUCompilerOptions.cmake),
  [`ErnicClangCompilerOptions.cmake`](cmake/ErnicClangCompilerOptions.cmake).
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
  another with `-DERNIC_USE_THREAD_SANITIZER=ON`. Run `ctest` with
  `export ASAN_OPTIONS=detect_leaks=1`. Every report is fatal. Default
  run-time options (including TSan's `halt_on_error=1`) are in
  [`sanitizer-default-options.c`](cmake/sanitizer-default-options.c);
  `*SAN_OPTIONS` override them. Sanitizer flags go only in
  `ErnicSanitizers.cmake`.
- **Valgrind-clean** (memcheck finds uninitialized reads, including
  uninitialized bytes passed to `send()`/`write()`, that sanitizers
  don't). Use a plain **GCC** build: not Clang, whose
  `-ftrivial-auto-var-init=pattern` hides those reads, and not a
  sanitizer build. Install valgrind before configuring so
  `<valgrind/valgrind.h>` is found (`tcp-thread-stop-unit` uses it to
  skip a `RLIMIT_NOFILE` row valgrind breaks). As CI's GCC Debug leg
  does:

  ```bash
  ctest --test-dir build -T memcheck --output-on-failure \
    --overwrite "MemoryCheckCommand=$(command -v valgrind)" \
    --overwrite "MemoryCheckCommandOptions=--leak-check=full --track-origins=yes --num-callers=40 --error-exitcode=1" \
    -E '^(pci-config-test|find-rdma-device-tuple|server-lifecycle|ionic-server-smoke|ionic-ci|tcp-mesh-ci|nvmeof-ci|s3-ci|ionic-stats-interval-cli|cmake-subproject)$'
  ```

  The excluded tests run scripts, CMake, or a server launcher. Reports
  go to `build/Testing/Temporary/MemoryChecker.*.log`, not ctest's
  output.

  CI then runs the server itself under valgrind. Test scripts run the
  server under `ERNIC_TEST_SERVER_WRAPPER` (split on whitespace); new
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
    -R '^(pci-config-test|ionic-server-smoke|ionic-ci|tcp-mesh-ci|nvmeof-ci|s3-ci|ionic-stats-interval-cli)$'
  valgrind --trace-children=yes $opts build/tests/test_server_lifecycle
  grep -L 'ERROR SUMMARY: 0 errors' build/valgrind-server/*.log
  ```

  Valgrind doesn't check writes into mapped guest memory; that relies on
  the `memset` rule in [Security](#security).
- **clang-tidy clean** with [`.clang-tidy`](.clang-tidy): every finding
  is an error. Needs clang-tidy 19+ and the same-version Clang (GCC's
  options are rejected), in its own build directory, Debug and Release
  (`NDEBUG` and `_FORTIFY_SOURCE` change the analysis). `-k 0` reports
  every failing file. CI
  ([clang-tidy.yml](.github/workflows/clang-tidy.yml)) uses clang-tidy
  23:

  ```bash
  cmake -B build-tidy -G Ninja -DCMAKE_C_COMPILER=clang-23 \
    -DERNIC_USE_CLANG_TIDY=ON -DERNIC_CLANG_TIDY_EXE=/usr/bin/clang-tidy-23
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
  `-fzero-init-padding-bits`. To check as CI
  ([iwyu.yml](.github/workflows/iwyu.yml)) does, starting from an empty
  build directory and `iwyu-runs` (IWYU skips up-to-date files):

  ```bash
  cmake -B build-iwyu -G Ninja -DCMAKE_C_COMPILER=clang-19 \
    -DERNIC_USE_IWYU=ON -DERNIC_IWYU_EXE="$PWD/ci/iwyu/iwyu-wrapper.sh"
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
  (`service/`, the driver pack's `setup-ionic-dkms.sh` and
  `fetch-ionic-sources.sh`, the Ansible collection's `files/`) keep
  `#!/bin/bash`.
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
  [`.github/actionlint.yaml`](.github/actionlint.yaml): self-hosted
  runner labels, and an ignore for the unsupported `$/` form of `uses:`.
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

## The QEMU code in `src/qemu/`

`src/qemu/` is the core of QEMU's PVRDMA device model, forked and heavily
rewritten. Upstream deleted it, so **it's ours to modify**, built and
checked like the rest of `src/`. Keep each file's original copyright
lines. History: [src/qemu/README.md](src/qemu/README.md).

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
Patches under `patches/` take the licenses of the Linux ionic and
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
[checks](#linters-warnings-and-sanitizers-mandatory).

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
`adminq_parse_roce_hdr()` in `src/ionic_adminq.c` builds). Once it knows
its node id it advertises them as `TCP_MSG_NODE_GIDS`; the manager fans
these out, and peers keep a `GID -> node id` table keyed on all 16
bytes.

`tcp_resolve_node_from_gid()` in
[`rdma_backend_tcp.c`](src/rdma/rdma_backend_tcp.c) is only an exact
lookup. A miss returns `UINT32_MAX`; `tcp_qp_state_rtr()` returns
`-EINVAL`, `rdma_rm_modify_qp()` turns it into `-EIO`, and the adminq
MODIFY_QP path fails the command, logging the full GID. **Never
reintroduce a default peer**: a wrong node is a silent write into another
guest's memory.

`service/rocm-ernic-launcher` derives each instance's IPv6 link-local GID
from its MAC (what the guest autoconfigures); override with
`ERNIC_GUEST_GIDS_<n>` when peers target a static address. Unlisted
addresses are unreachable. Reproducing a failure needs three or more
nodes and the `-T` TAP plus bridge setup. Covered by
`tests/test_tcp_gid_resolve.c`.

### Dead and restarted mesh nodes

Nodes are never evicted from `mesh_nodes`; a dead one is marked
`is_alive = false`. A node that went quiet returns under its id, but a
restarted one registers again and gets a new id (ids are monotonic, so
not stable across runs). Both ids then claim the same GIDs.

So `TcpNodeGids` has an `alive` flag, which the manager clears when it
declares a node dead and sets when it answers again, and
`tcp_gid_index_rebuild()` skips non-alive claimants: the returning guest
owns its addresses under its new id, and a late node regains them
without re-advertising. Two *live* claimants still resolve to nothing.
Workers keep no liveness of their own; the manager advertises a dead node
as owning nothing (`tcp_broadcast_gid_table()`), which is what gives an
empty advertisement meaning.

Don't make the index read `is_alive` from `mesh_nodes`: that takes
`mesh_table_lock` under `gid_table_lock`, the reverse of the lock order.
Covered by `test_dead_node_stops_owning_its_gids()` in
`tests/test_tcp_gid_resolve.c` and the churn rounds in
`tests/py/test_mesh_stress.py`.

### Connection lifetime

- A manager reaps connections that never registered
  (`tcp_reap_pending_conns()`) and retires a registered node's connection
  when it hangs up (`tcp_health_check_retire()`). A worker closes a dead
  connection to another worker on its peer-connect thread's next pass
  (`tcp_worker_peer_sweep()`), and they reconnect; the accept thread and
  `tcp_worker_publish_peer()` replace a dead connection, never a live
  one. `TcpConnection` is reference counted, so the data path may hold
  one after it leaves its table; see its comment for the rules.
- A failed `accept()` (e.g. `EMFILE`, which leaves the connection queued)
  is retried after 100 ms. Without that backoff it once logged 295 GB.
- A registration wall appears at about 40 nodes (measured on 64). The
  full-mesh connection cost behind it is inferred, **not** confirmed in
  the code.

### Trap: the mesh has no Ethernet or DHCP path

Guest TCP/IP uses the host TAP/bridge
([`ionic_eth_net.c`](src/ionic_eth_net.c)). The mesh carries only
`TCP_MSG_IONIC` and control traffic. The old never-used Ethernet and
DHCP relay paths are gone, but their ordinals remain as
`TCP_MSG_RESERVED_18..20`: the protocol has no versioning beyond a magic
number, so renumbering would misparse against an unupgraded peer. Don't
rebuild guest networking inside the mesh.
