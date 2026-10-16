# AGENTS.md

Guidance for AI coding agents working in the rocm-ernic repository.
rocm-ernic is a userspace emulated RDMA NIC for virtual machines, built on
libvfio-user; see [README.md](README.md) for scope,
[INSTALL.md](INSTALL.md) for dependencies and CMake options, and
[docs/testing.rst](docs/testing.rst) for the test programs.

## Project direction

rocm-ernic will move into the
[rocm-systems](https://github.com/ROCm/rocm-systems) monorepo and then be
built by [TheRock](https://github.com/ROCm/TheRock), ROCm's build system.
Make changes that will survive that move:

- **Ask before changing compile or link options, dependency handling, or
  any other non-trivial CMake.** A superproject build has requirements
  that a standalone build doesn't show, so a change that works here can
  break there. For example, `ERNIC_WERROR` is off by default so that a
  downstream build with a compiler we haven't tried isn't broken by a new
  warning; CI turns it on.
- **Don't assume rocm-ernic is the top-level CMake project.** In new CMake
  code, use `PROJECT_SOURCE_DIR`/`PROJECT_BINARY_DIR` or
  `CMAKE_CURRENT_SOURCE_DIR`/`CMAKE_CURRENT_BINARY_DIR` rather than
  `CMAKE_SOURCE_DIR`/`CMAKE_BINARY_DIR` (existing code, especially
  `tests/CMakeLists.txt`, still uses the latter in many places).
- **Namespace what you add.** Prefix new cache variables with `ERNIC_` and
  new targets with `ernic-`. Several existing targets (`doxygen`,
  `sphinx-html`, the `test_*` programs) have generic names that will
  collide in a superproject; don't add more like them.
- **Set flags per target**, through `ernic_set_compiler_flags()` and
  `ernic_add_sanitizers()` in [`cmake/`](cmake/), not globally with
  `add_compile_options()` or `CMAKE_C_FLAGS`.

### Sister projects

The same team writes two sister projects:

- **hipObject** ([ROCm/hipObject](https://github.com/ROCm/hipObject)), an
  RDMA-accelerated S3 object client for AMD GPUs. Its emulated-hardware
  CI runs against rocm-ernic's ionic NIC, and rocm-ernic's S3 target
  speaks its protocol (see [docs/s3.rst](docs/s3.rst)).
- **hipFile**, GPU-direct file I/O, in rocm-systems under
  [`projects/hipfile`](https://github.com/ROCm/rocm-systems/tree/develop/projects/hipfile).
  It already lives in rocm-systems and builds with TheRock, so it's the
  reference for how rocm-ernic should look after the move.
  `cmake/ErnicSanitizers.cmake` already follows its pattern.

Borrow ideas from them freely. Ideas can flow the other way too. If you
find something in rocm-ernic that one of them should adopt, or a bug or
improvement in them, ask whether to create a to-do file for it; don't
create one or edit the other project without asking.

## Configure and build

libvfio-user is built from source; [INSTALL.md](INSTALL.md) has the steps
and the other dependencies, and CI pins the commit it builds in
[build-and-test.yml](.github/workflows/build-and-test.yml). If it's
installed somewhere other than `/usr` or `/usr/local`, pass the prefix
with `-DCMAKE_PREFIX_PATH`.

From the repository root, match CI:

```bash
cmake -B build -G Ninja -DERNIC_WERROR=ON
cmake --build build
```

Notes:

- The default build type is Debug.
- `ERNIC_WERROR` is off by default (see
  [Project direction](#project-direction)). Without it, warnings don't
  fail the build, so either turn it on or read the build output.
- CI builds every combination of GCC and Clang, Debug and Release, and no
  sanitizer, ASan, and TSan, plus one Clang coverage build. Release is not
  just Debug at a higher optimization level: it's the only configuration
  with GCC's dataflow warnings (which need the optimizer), with
  `_FORTIFY_SOURCE=3`, and with `NDEBUG`. Build it as well as Debug.
- The documentation builds without the C dependencies:
  `-DERNIC_DOCS_ONLY=ON -DERNIC_BUILD_DOCS=ON` (see
  [Spelling and documentation](#spelling-and-documentation)).

## Tests

- After a successful build: `ctest --test-dir build --output-on-failure`.
  CI then also runs `tests/test_loopback_ci.sh` against the server and
  `tests/test_data_transfer`; see the workflow for the environment it
  needs.
- **Every registered test runs without a VM, RDMA hardware, or root**, and
  CI runs them all on hosted runners, so a skipped test means something is
  wrong. Multi-VM and real-hardware testing is described in
  [docs/testing.rst](docs/testing.rst) and run by the `system-tests`,
  `nvmeof-lane`, `s3-lane`, and `self-hosted-ci` workflows.
- The unit tests (`*-unit`) are always built with ASan, LSan, and UBSan
  unless the project is built with TSan (`ERNIC_UNIT_TEST_SAN` in
  `tests/CMakeLists.txt`), so a plain Debug build already sanitizes them.
- **Run a TSan build's tests under `setarch "$(uname -m)" -R`**, as CI
  does. Without it, nearly every test aborts before `main()` with
  `FATAL: ThreadSanitizer: unexpected memory mapping`. That is address
  space randomization colliding with TSan's shadow memory, not a test
  failure.
- **Under a full ASan build (`-DERNIC_USE_SANITIZERS=ON`), the tests that
  start the real server can reach their CTest timeouts**: `nvmeof-ci`,
  `s3-ci`, `ionic-ci`, `ionic-server-smoke`, and `pci-config-test`. A
  timeout there is not evidence against a change. A real failure, or a
  timeout in a build without sanitizers, is.
- **Code coverage of `src/` must not fall below the floor** set by
  `MIN_LINE_COVERAGE` in
  [build-and-test.yml](.github/workflows/build-and-test.yml), which is
  raised as tests are added. CI's coverage build fails below it. Add tests
  with new or changed code, including its error paths. To measure it,
  configure with Clang and `-DERNIC_USE_CODE_COVERAGE=ON`, run `ctest`,
  and build the `ernic-coverage` target, which writes
  `coverage/report.txt` and `coverage/lines.txt` in the build directory.
  The profiles accumulate across runs; delete `coverage/profraw` to start
  over.

## C code

- C code is standard C11. Do not use GNU extensions without permission.
- Assume that atomics and multithreading are available: `<stdatomic.h>`,
  and threads and mutexes through the `qemu/thread.h` wrappers over
  pthreads. See [Thread safety](#thread-safety).
- **Prefer explicitly sized integer types** (`uint32_t`, `uint64_t`, and
  `size_t` for sizes) instead of `int`, `long`, etc. The width of `long`
  depends on the platform, so never use it for a value that goes on the
  wire or has to be 64 bits. Print with the `<inttypes.h>` macros
  (`PRIu64`, `PRIx32`) and `%zu`, not `%lu` or `%ld`, and write 64-bit
  constants with `UINT64_C()`, not an `L` or `UL` suffix.
- **Don't convert numbers with the C library's functions directly**: the
  `ato*` family, the `strto*` family, or `sscanf()`. The `ato*` functions
  can't report an error, `sscanf()` doesn't say whether it consumed all the
  text, and the `strto*` functions report errors only through `errno` and
  an end pointer that are easy to get wrong. Use `ernic_parse_int()` and
  `ernic_parse_port()` from [`src/parse_int.h`](src/parse_int.h), which
  reject empty text, trailing characters, and out-of-range values. For
  another type, add a helper there, with tests in
  `tests/test_parse_int.c`. Some existing code still calls `strto*`
  directly; don't copy it.

## Thread safety

**Races in this tree already cause intermittent CI failures.** Treat thread
safety as a correctness requirement on every change that touches shared
state.

The server's main thread runs the vfio-user context and the poll loop, and
only it may DMA into guest memory. The TCP backend
([`rdma_backend_tcp.c`](src/rdma/rdma_backend_tcp.c)) adds an accept
thread, a health-check thread, and a receive thread per connection, all
started with `qemu_thread_create()`.

- **Anything one thread writes and another reads needs a lock or an
  atomic.** Use `QemuMutex` (`qemu/thread.h`) or `<stdatomic.h>`. A plain
  variable shared without either is a data race, which is undefined
  behavior: the compiler is free to hoist a load out of a polling loop, so
  a thread may never see a stop flag, and the join that waits for it
  hangs.
- **`volatile` is not synchronization.** Its one legitimate use here is
  `volatile sig_atomic_t` for a flag set by a signal handler.
- **Don't use `qatomic_*()` for shared data.** They aren't atomic (see
  [Known defects and traps](#trap-qatomic_-are-not-atomic)).
- **Explain any memory order other than the default.** Plain atomic
  operations are sequentially consistent, which is always correct. If you
  use `memory_order_relaxed` or acquire/release, comment why it's enough,
  as the comment on `recv_thread_running` in `rdma_backend_tcp.c` does.
- **Document which lock protects each shared field**, next to the field.
  If code takes more than one lock, document the order and always take
  them in that order.
- **Don't hold a lock across a blocking call**, such as socket I/O, a
  sleep, or `qemu_thread_join()`, or across a call that might take the
  same lock.
- **Every thread you start must be stoppable and joined.** Give it an
  atomic stop flag that it polls with a bounded timeout, and join it at
  teardown. A thread that never returns hangs shutdown. If it blocks, the
  stop has to wake it: `tcp_stop_accept_thread()` in
  `rdma_backend_tcp.c` shuts down the socket a thread is blocked on, and
  `tcp_stop_health_check_thread()` signals the condition variable a thread
  waits on between passes.
- **Test concurrency with TSan, in both Debug and Release.** Configure
  with `-DERNIC_USE_THREAD_SANITIZER=ON`, which can't be combined with
  `ERNIC_USE_SANITIZERS`. For Debug, also pass
  `-DCMAKE_C_FLAGS_DEBUG="-g -Og"`, as CI does: at `-O0` the compiler
  doesn't reorder enough to expose the races an optimized build hits. Run
  the tests under `setarch` (see [Tests](#tests)).
- **A passing test doesn't prove TSan was quiet.** The scripts that start
  the server don't all fail on a report from the server process. Search
  `Testing/Temporary/LastTest.log` in the build directory for
  `WARNING: ThreadSanitizer` after the run.
- **TSan can't see the guest.** Memory shared with the guest, such as
  queue rings and their indices, is written by another process, so TSan
  never reports a race on it. Accesses to it need atomics or fences chosen
  by reasoning about the guest driver, not by a clean TSan run.
- **A flaky test is a suspected race until shown otherwise.** Don't retry
  it, lengthen its timeout, or add a sleep to make it pass; find the race.

## Security

**Security is critical.** The emulator is a boundary between guest and
host. A bug here can let a guest read or write host memory or another
guest's memory, or take down a server that other guests depend on, so
security takes priority over speed and convenience.

- **Treat everything from outside the process as untrusted.** That
  includes everything the guest controls (BAR and doorbell writes, admin
  queue commands, queue descriptors and ring indices, and the guest
  addresses, lengths, keys, and QP numbers in them), mesh peers on the TCP
  backend, and network clients of the NVMe-oF and S3 targets. Check
  lengths, indices, and ranges before using a value, and reject malformed
  input with an error rather than truncating, clamping, or guessing. Never
  act on an address, key, or length from the guest or a peer without
  checking it against what was registered.
- **Integer arithmetic must be safe.** Be very careful of integer security
  issues.
  - Check for wrap-around before it happens, not after. `offset + len` can
    wrap, so compare against the limit first
    (`len > limit || offset > limit - len`). The `__builtin_*_overflow`
    functions are GNU extensions, so ask before using them.
  - Check ranges before narrowing. Do not blindly cast to clear
    warnings.
  - Avoid undefined behavior: signed overflow, shifts by a negative amount
    or by the type's width or more, and conversions of out-of-range values
    to signed types.
  - UBSan catches signed overflow, but unsigned wrap-around is defined
    behavior, so no sanitizer reports it. That part is on you.
- **Bound every buffer.** No `strcpy`, `sprintf`, or `strcat`; use
  `snprintf` and check for truncation.
- **Any data intended for the wire MUST be cleared before use.** That
  covers data sent to the guest (completions, admin queue responses,
  anything DMAed into guest memory) as well as to network peers. `memset`
  the whole object to zero before filling it in. An initializer such as
  `= {0}` isn't guaranteed to zero padding, and padding carries stale
  memory onto the wire. Give a new wire structure a `_Static_assert` on
  its size, so padding added by a later edit fails the build.
- **Keep the hardening flags.** Don't remove or weaken `_FORTIFY_SOURCE`
  ([`ErnicFortifySource.cmake`](cmake/ErnicFortifySource.cmake)),
  `-fstack-protector-strong`, `-fstack-clash-protection`, or the linker
  hardening in `ernic_set_linker_flags()`
  ([`ErnicCompilerOptions.cmake`](cmake/ErnicCompilerOptions.cmake)): PIE,
  full RELRO, and a non-executable stack. Ask before
  changing any compile or link options (see
  [Project direction](#project-direction)).
- **Fix security scan findings.** CodeQL
  ([codeql.yml](.github/workflows/codeql.yml)) analyzes the C code and the
  PR security scan
  ([pr-security-scan.yml](.github/workflows/pr-security-scan.yml)) runs on
  every PR. Do not introduce new alerts. Fix findings rather than
  dismissing them, and ask before dismissing one.

## Linters, warnings, and sanitizers (mandatory)

**Every change must pass every check below that covers the files it
touches. These checks are not optional or advisory.** A change with an
outstanding finding isn't finished. Several CI workflows only run when
files matching a path filter change (Build and Test, for example, doesn't
run for a change only to `shared/`), and some checks aren't in CI at all,
so a green PR doesn't prove a change is clean. Run the checks yourself.

Fix the code rather than suppressing a finding. Only suppress a finding
that is unavoidable, for example because of vendored code or the way a
framework works, as narrowly as possible, with a comment saying why.

Run the commands below from the repository root, which is where CI runs
them and where their configuration files are picked up.

### C

- **clang-format 18**, with [`.clang-format`](.clang-format), over every
  `.c` and `.h` outside `third-party/`. Check without changing anything:

  ```bash
  git ls-files -z -- '*.c' '*.h' ':!:third-party/' \
    | xargs -0 clang-format-18 --dry-run --Werror
  ```

  Reformat with `clang-format-18 -i`. CI:
  [clang-format.yml](.github/workflows/clang-format.yml).
- **No compiler warnings, with both GCC and Clang**, in Debug and Release.
  They warn about different things, so build with each, in separate build
  directories (`-DCMAKE_C_COMPILER=gcc`, and `clang`), with
  `-DERNIC_WERROR=ON`. The warning lists are in
  [`ErnicGNUCompilerOptions.cmake`](cmake/ErnicGNUCompilerOptions.cmake)
  and
  [`ErnicClangCompilerOptions.cmake`](cmake/ErnicClangCompilerOptions.cmake).
- **Suppress a warning only with the macros in
  [`shared/rocm-ernic-warnings.h`](shared/rocm-ernic-warnings.h)**, never
  with raw `#pragma`s, `-Wno-…` flags, or changes to the warning lists, and
  only for warnings that are almost impossible to fix due to things like
  outside or vendored code. To add one, define a
  `ROCM_ERNIC_WARN_<NAME>_OFF`/`_ON` pair there like the existing ones,
  add both to `StatementMacros` in [`.clang-format`](.clang-format), and
  wrap the smallest region you can. If you remove a suppression, check
  whether its macros are used elsewhere, so you don't leave orphaned
  macros behind.
- **Sanitizer-clean**: the tests must run without a report from ASan,
  LSan, UBSan, or TSan. Use one build directory for
  `-DERNIC_USE_SANITIZERS=ON` (ASan, LSan, and UBSan together) and another
  for `-DERNIC_USE_THREAD_SANITIZER=ON`, and run `ctest` in each with the
  options CI uses:

  ```bash
  export ASAN_OPTIONS=detect_leaks=1
  export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
  export TSAN_OPTIONS=halt_on_error=1
  ```

  UBSan otherwise prints its report and exits successfully, so a test can
  pass with undefined behavior in its output. See [Tests](#tests) for the
  TSan `setarch` wrapper and the ASan timeouts, and
  [Thread safety](#thread-safety) for the TSan build flags.
- **clang-tidy clean**: building with `-DERNIC_USE_CLANG_TIDY=ON` runs
  clang-tidy on every file as it compiles, with the checks in
  [`.clang-tidy`](.clang-tidy), and must report nothing; every finding is
  an error. It needs CMake 3.27 or later, clang-tidy 19 or later, and
  Clang as the C compiler (configuring fails with GCC, whose options
  clang-tidy rejects). Use the clang of the same version as clang-tidy,
  in a separate build directory, and build Release as well as Debug,
  since `NDEBUG` and `_FORTIFY_SOURCE` change what the analyzer sees:

  ```bash
  cmake -B build-tidy -G Ninja -DCMAKE_C_COMPILER=clang-23 \
    -DERNIC_USE_CLANG_TIDY=ON -DERNIC_CLANG_TIDY_EXE=/usr/bin/clang-tidy-23
  cmake --build build-tidy -- -k 0
  ```

  `-k 0` keeps going after a file fails, so one build reports every
  finding. The vendored sources in `third-party/` and their headers are
  skipped.

  `.clang-tidy` turns on whole groups of checks, leaving out the groups
  for other platforms and code bases (listed there with the reason), and
  then turns off, marked `TODO`, the checks the current code doesn't pass
  yet. Don't add a check to that list to silence a new finding; fix the
  code. When you fix the code for a `TODO` check, delete its line and the
  lines for its aliases (listed after it), so it stays on. Suppress a
  finding only when it's unavoidable, with `// NOLINTNEXTLINE(check-name)`
  naming the one check, and a comment saying why.

  CI: [clang-tidy.yml](.github/workflows/clang-tidy.yml), with upstream
  clang-tidy 23, in Debug and Release.
- **include-what-you-use (IWYU)** (`apt install iwyu`; needs CMake 3.27 or
  later): it prints the `#include` lines it suggests adding or removing for
  each file as the build compiles it. IWYU parses with its own clang, which
  rejects options it doesn't know, such as GCC 15's
  `-fzero-init-padding-bits`, so configure a separate build directory with
  the clang that IWYU is built on, whose version
  `include-what-you-use --version` prints. To see the suggestions in the
  build output:

  ```bash
  cmake -B build-iwyu -G Ninja -DCMAKE_C_COMPILER=clang-19 \
    -DERNIC_USE_IWYU=ON
  cmake --build build-iwyu
  ```

  To check the build as CI does, which collects every IWYU run, fails if
  IWYU itself failed on any file (a parse error or an option it rejected),
  and lists the files with suggestions, run IWYU through the wrapper:

  ```bash
  cmake -B build-iwyu -G Ninja -DCMAKE_C_COMPILER=clang-19 \
    -DERNIC_USE_IWYU=ON -DERNIC_IWYU_EXE="$PWD/ci/iwyu/iwyu-wrapper.sh"
  mkdir build-iwyu/iwyu-runs
  ERNIC_IWYU_LOG_DIR="$PWD/build-iwyu/iwyu-runs" cmake --build build-iwyu
  ci/iwyu/check-iwyu.sh build-iwyu/iwyu-runs
  ```

  The build doesn't rerun IWYU on files that are already up to date, so
  start from an empty build directory, or `ninja -C build-iwyu -t clean`,
  and an empty `iwyu-runs`. The vendored sources in `third-party/` are
  skipped.

  Unlike the other checks, IWYU doesn't have to report nothing, since some
  of its suggestions are wrong. Make the suggestions that are right for the
  files you touch, such as adding a header for a symbol the file uses but
  only gets indirectly. That includes removing `<stdint.h>` from a file
  that includes `<inttypes.h>`, which C11 requires to include it. Ignore
  the wrong ones and leave the code as it is. These are wrong:
  - Adding a forward declaration, such as `struct nvmeof_cm;`, of a
    module's opaque type to a file that includes the module's header,
    which already declares it.
  - Anything for a test that `#include`s the translation unit under test
    (for example, `test_tcp_mesh_topology.c` includes
    `rdma/rdma_backend_tcp.c`). The test gets its declarations from that
    file's includes on purpose, and IWYU asks it to repeat them.
  - Naming an internal header instead of the public one, or insisting on
    one of several headers that repeat a typedef (C11 allows that, and the
    QEMU compatibility headers in `src/qemu-compat/include/` do it for
    `PCIDevice`, `dma_addr_t`, and others). Fix these with a mapping in
    [`cmake/iwyu-libc.imp`](cmake/iwyu-libc.imp) or
    [`cmake/iwyu-ernic.imp`](cmake/iwyu-ernic.imp), or with an
    `// IWYU pragma: export` on the include in the header that should
    provide the symbol, as `hw/pci/pci.h` does for `hw/pci/pci_device.h`.

  CI ([iwyu.yml](.github/workflows/iwyu.yml)) runs IWYU but only fails if
  IWYU itself stops working. It lists the files with suggestions in the job
  summary without failing.
- **Static analysis** (clang-tidy, cppcheck, flawfinder, and the Clang
  static analyzer, through the Nix toolkit in `nix/analysis/`) runs on a
  PR when it has the `run-static` label
  ([static-analysis.yml](.github/workflows/static-analysis.yml)). It's
  informational and never fails, but read its findings for the files you
  touched. Its clang-tidy run uses its own set of checks, not
  `.clang-tidy`, and includes the vendored code, so it doesn't replace the
  clang-tidy check above.

### Shell

bash scripts must be shellcheck-clean.

- **ShellCheck 0.11.0** over every `*.sh`, `*.sh.in`, and extension-less
  executable with a shell shebang, outside `third-party/`, with the checks
  CI excludes:

  ```bash
  export SHELLCHECK_OPTS='-e SC2024,SC2034,SC2086,SC2154,SC2164,SC2181,SC2317,SC2329'
  git ls-files -- ':!:third-party/' | while IFS= read -r f; do
    if [[ "$f" == *.sh || "$f" == *.sh.in ]]; then
      echo "$f"
    elif [[ "${f##*/}" != *.* && -x "$f" ]] &&
      head -n1 -- "$f" | grep -Eqs '^#!.*\b(ba)?sh\b'; then
      echo "$f"
    fi
  done | xargs -d '\n' shellcheck
  ```

  CI: [shellcheck.yml](.github/workflows/shellcheck.yml).
- **The shell in workflow `run:` blocks must be clean too**, and the
  ShellCheck workflow doesn't look at it. Check it with
  [actionlint](https://github.com/rhysd/actionlint)
  (`pip install actionlint-py`), which runs shellcheck on each `run:`
  block, with the same `SHELLCHECK_OPTS` exported:

  ```bash
  actionlint -ignore 'label ".+" is unknown'
  ```

  The `-ignore` drops the reports about the self-hosted runners' custom
  labels. The existing workflows aren't clean yet, so check the blocks you
  add or change. actionlint doesn't read the composite actions in
  `.github/actions/`, and neither tool sees the bodies of heredocs, such
  as the scripts the system tests run inside guest VMs; hold those to the
  same standard by hand. Nothing in CI runs actionlint.

### CMake

- **cmakelint**, with [`.cmakelintrc`](.cmakelintrc):

  ```bash
  git ls-files -z -- 'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake' \
    ':!:third-party/' | xargs -0 cmakelint
  ```

  CI: [cmakelint.yml](.github/workflows/cmakelint.yml).

### Python

Python code must run on **Python 3.10**. Don't use anything newer, such
as `tomllib`, `except*`, or `typing.Self` (all 3.11).

- **black** and **pylint**, over every `*.py` and extension-less script
  with a python shebang, except `third-party/`, `nix/analysis/triage/`,
  and `.cmake-format.py`. pylint needs
  `prometheus/requirements-exporter.txt` installed to resolve
  ernic-exporter's imports.

  ```bash
  git ls-files -- ':!:third-party/' ':!:nix/analysis/triage/' \
    ':!:.cmake-format.py' | while IFS= read -r f; do
    if [[ "$f" == *.py ]]; then
      echo "$f"
    elif [[ "${f##*/}" != *.* ]] &&
      head -n1 -- "$f" | grep -Eqs '^#!.*python'; then
      echo "$f"
    fi
  done > /tmp/python-files
  xargs -d '\n' black --check < /tmp/python-files
  xargs -d '\n' pylint < /tmp/python-files
  ```

  CI: [pylint.yml](.github/workflows/pylint.yml).
- **Suppress pylint findings as narrowly as possible**
  (`# pylint: disable-next=...` on the one line), with a comment saying
  why.
- **vermin**, since CI runs black and pylint under Python 3.12 and neither
  catches most uses of newer language features or modules
  (`pip install vermin`). It exits nonzero and prints
  `Target versions not met` if the code needs a newer Python:

  ```bash
  xargs -d '\n' vermin -t=3.10- --violations --no-tips < /tmp/python-files
  ```

  Nothing in CI runs vermin.

### Spelling and documentation

- **codespell**, with [`.codespellrc`](.codespellrc), over the whole tree,
  Markdown included: `codespell`. CI:
  [codespell.yml](.github/workflows/codespell.yml), on every PR.
- **doc8** for reStructuredText: `doc8 docs/ --max-line-length 80`. The
  documentation must also build, which needs Doxygen but not the C
  dependencies:

  ```bash
  cmake -B build-docs -DERNIC_DOCS_ONLY=ON -DERNIC_BUILD_DOCS=ON
  cmake --build build-docs --target sphinx-html
  ```

  CI: [docs-check.yml](.github/workflows/docs-check.yml).

### Ansible

- **ansible-lint** and **`ansible-test sanity`** on the collection in
  `ansible/`. [ansible-collection.yml](.github/workflows/ansible-collection.yml)
  has the exact commands, including installing the collection's
  requirements first. CI runs them when something under `ansible/`
  changes.

## Vendored code

Code in `third-party/` does not belong to AMD and should not be modified,
including formatting.

## Copyright lines

AMD copyright lines do not carry a year. Write:

```
Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
```

not `Copyright (c) 2026 Advanced Micro Devices, Inc.`. Do not add a year to a
new file, and do not "update" a year on an existing one.

Two forms are in use, split by license:

- MIT-licensed files — the build system and the deployment and automation
  code (`CMakeLists.txt`, `cmake/`, `docs/`, `ansible/`, `ci/`, `.github/`,
  `prometheus/`, `udev/`, `scripts/`, `service/`), plus the headers in
  `shared/` — use the full line above, matching `LICENSE.md`.
- GPL-licensed files — everything under `src/` and `tests/`, and the fuzz
  harnesses in `nix/analysis/fuzz/` — use
  `Copyright (C) Advanced Micro Devices, Inc.` — capital `(C)`, no
  "All rights reserved" — and keep the QEMU GPL boilerplate that follows it.

Third-party copyright lines are left exactly as they arrived, years included.

## Changelog

**Every change that someone deploying, running, or contributing to
rocm-ernic could notice needs an entry in [CHANGELOG.md](CHANGELOG.md), in
the same change.** That includes what the guest sees of the emulated
device, the server's command-line options and environment variables,
`ernicctl`, the service and udev files, the ernic-exporter metrics, the
protocol between mesh nodes, CMake options, installed files and packages,
dependencies, and notable CI and test additions. Purely internal
refactoring doesn't need one.

Add the entry to the `UNRELEASED` section, under the matching heading:
`Added`, `Changed`, `Removed`, `Fixed`, `Limitations`, or `Known issues`.
Write it for users, like the existing entries: say what changed, and what
users have to do differently, if anything. Start an entry that breaks
existing deployments with `**Breaking:**`.

The Ansible collection is versioned separately and has its own changelog.
For a change to it, add a fragment to `ansible/changelogs/fragments/`, using
the sections in `ansible/changelogs/config.yaml`, instead.

## Lab provisioning

To set up guests and hosts, use the Ansible collection; see
[ansible/README.md](ansible/README.md).

## Known defects and traps

### Fixed: the TCP backend resolved every GID to the same node

Inter-node RDMA over the TCP mesh backend was broken above two nodes.
Every destination GID resolved to one node id, so traffic for nodes 2+
was delivered to node 0 (worker mode) or node 1 (everything else), with
no error anywhere.

#### What was wrong

`tcp_resolve_node_from_gid()` derived a node id from one octet of the
destination GID by two narrow rules, and its callers substituted a
hardcoded default when neither matched:

```c
if (resolved != UINT32_MAX) {
    tqp->remote_node_id = resolved;
} else if (priv->mode == TCP_MODE_WORKER) {
    tqp->remote_node_id = 0;
} else {
    tqp->remote_node_id = 1;
}
```

The default was constant per instance and independent of the GID, which
is why one run showed three distinct GIDs producing one answer and two
instances disagreeing about which node that was.

Both rules were guesses. One read `raw[15]` as a node id, which only
holds for the synthetic GID built in `tcp_add_gid()` — and nothing calls
`rdma_rm_add_gid()`, so that GID is never registered. The other decoded
an IPv4-mapped GID under a `<subnet>.<(node_id + 1) * 10>` convention,
but [the DHCP pool][pool] allocated sequentially from `.10`, so only the
first node ever matched. The GIDs actually seen in the field were IPv6
link-local, which the first rule rejected outright.

#### How it works now

Node identity is configured, not inferred. Each instance is told its own
guest RoCE addresses in `ERNIC_TCP_GUEST_GIDS` (comma-separated,
`inet_pton` notation; IPv4 is stored in the `::ffff:a.b.c.d` mapped form
[`adminq_parse_roce_hdr()`][parse] builds). It advertises them over the
mesh as `TCP_MSG_NODE_GIDS` once it knows its node id, the manager fans
each advertisement out to the rest, and peers keep a `GID -> node id`
table keyed by the full 16 bytes.

[`tcp_resolve_node_from_gid()`][resolve] is now nothing but an exact
lookup. A miss returns `UINT32_MAX` and the caller fails:
[`tcp_qp_state_rtr()`][rtr] returns `-EINVAL`, which
`rdma_rm_modify_qp()` turns into `-EIO`, and
[the adminq MODIFY_QP path][adminq] fails the command with the full GID
in the log. **Do not reintroduce a default peer**: a wrong node here is a
silent write into another guest's memory that nothing downstream catches.

Deployment side: `service/rocm-ernic-launcher` derives each instance's
IPv6 link-local GID from its MAC, which is what a guest autoconfigures.
Override with `ERNIC_GUEST_GIDS_<n>` when the guest also carries a static
address peers will target. An address not listed is unreachable.

Covered by `tests/test_tcp_gid_resolve.c`.

#### Reproducing the original failure

Needs a minimum of three nodes. The `-T` TAP plus bridge setup is
required; without it the run never reaches the data path.

### Trap: the mesh has no Ethernet or DHCP path

Guest TCP/IP rides the host TAP/bridge ([`ionic_eth_net.c`][ethnet] is a
plain TAP open/send/recv). The mesh carries only `TCP_MSG_IONIC` plus
control traffic.

It used to also carry raw Ethernet frames and relay DHCP, but nothing
ever sent either — both were handled and never originated. That dead code
is gone, along with `src/net/dhcp_server.c` and `src/net/eth_rx_inject.c`.
Their message ordinals survive as `TCP_MSG_RESERVED_18..20`: the wire
protocol has no version negotiation beyond a magic number, so renumbering
what follows would silently misparse against an unupgraded peer.

If you need guest-visible networking, the TAP/bridge is the path. Do not
rebuild a second one inside the mesh.

### Trap: `qatomic_*()` are not atomic

The `qatomic_*()` functions in
[`src/qemu-compat/qemu_stubs.c`][stubs] are plain loads, stores, and
read-modify-writes on an `int`, written when the device logic was
single-threaded. Their only remaining callers are in the vendored QEMU
code in `third-party/qemu/hw/rdma/`, which is safe only because that
code's completion thread (started by `rdma_backend_start()`) is never
started.

Use `<stdatomic.h>` for anything shared between threads. Don't start the
vendored completion thread without first making these real atomics.

### Adjacent issues

Observed alongside the GID bug. The first two have since been traced in
the source; the rest have not:

- `CLOSE_WAIT` socket leak. Nothing closed a connection whose peer hung
  up: its receive thread just returned. A manager now reaps connections
  that never registered (`tcp_reap_pending_conns()`) and retires a
  registered node's connection when it hangs up
  (`tcp_health_check_retire()`), taking it out of the connections table. A
  worker still keeps a dead connection to another worker, and the pair
  never reconnect. `TcpConnection` is reference counted now, so a worker
  can take a dead connection out of its table while the data path still
  holds it; see the comment on `TcpConnection` for the rules.
- An `EMFILE` retry loop with no backoff. One run produced 295 GB of
  logs. This was the accept thread: an `accept()` that fails for lack of
  descriptors leaves the connection queued, so the next one fails at once
  too, and each failure was logged. A failed `accept()` is now retried
  after 100 ms. The leak above is the likely way the process ran out of
  descriptors.
- A registration wall at roughly 40 nodes. The number is measured on a
  64-node run; the full-mesh connection-cost mechanism behind it was
  inferred from socket counts and a slot-freeing experiment and is
  **not** confirmed against the code.
- Node ids are assigned monotonically, so they are not stable across
  runs.

Note that nodes are never evicted from `mesh_nodes` — a dead node is
marked `is_alive = false` and reconnects under the same id — so the GID
table has no removal path by design. A returning node re-advertises and
replaces its own entries.

[resolve]: src/rdma/rdma_backend_tcp.c
[rtr]: src/rdma/rdma_backend_tcp.c
[parse]: src/ionic_adminq.c
[adminq]: src/ionic_adminq.c
[pool]: src/rocm_ernic_compat.c
[ethnet]: src/ionic_eth_net.c
[stubs]: src/qemu-compat/qemu_stubs.c
