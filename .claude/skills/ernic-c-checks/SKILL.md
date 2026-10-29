---
name: ernic-c-checks
description: "Build and check a rocm-ernic C change before finishing it: the GCC and Clang warning builds, clang-tidy, the clang 23 thread-safety build, include-what-you-use, and line coverage of src/. Use after editing C in src/ or tests/, when changing #includes or locking annotations, when CI's build, clang-tidy, thread-safety, IWYU, or coverage job fails, or when asked to run the local checks."
---

# C checks

The rules these checks enforce are in [AGENTS.md](../../../AGENTS.md)
(see its Checks section); this file says how to run them. Run everything
from the repository root, each configuration in its own `build-<config>`
directory, and reuse the directories: a rebuild recompiles, and re-runs
clang-tidy or IWYU on, only what changed. `-k 0` makes ninja report every
failing file rather than stopping at the first.

## Warning builds

Every C change needs a GCC Debug build and its tests, and GCC Release and
Clang builds (building is enough), all with `-DERNIC_WERROR=ON`. CI's
main matrix uses Ubuntu 26.04's GCC 15 and Clang 21; its Ubuntu 24.04
smoke legs use GCC 13 (Release) and Clang 18 (Debug). Build Release with
GCC 13 if it's installed: its dataflow warnings have failed CI where GCC
15's passed. Release is the only configuration with GCC's dataflow
warnings, `_FORTIFY_SOURCE=3`, and `NDEBUG`.

```bash
cmake -B build-gcc -G Ninja -DCMAKE_C_COMPILER=gcc-15 \
  -DCMAKE_BUILD_TYPE=Debug -DERNIC_WERROR=ON -DERNIC_BUILD_P4=ON
cmake --build build-gcc
ctest --test-dir build-gcc -j"$(nproc)" --output-on-failure

cmake -B build-gcc-release -G Ninja -DCMAKE_C_COMPILER=gcc-13 \
  -DCMAKE_BUILD_TYPE=Release -DERNIC_WERROR=ON -DERNIC_BUILD_P4=ON
cmake --build build-gcc-release -- -k 0

cmake -B build-clang -G Ninja -DCMAKE_C_COMPILER=clang-21 \
  -DCMAKE_BUILD_TYPE=Debug -DERNIC_WERROR=ON -DERNIC_BUILD_P4=ON
cmake --build build-clang -- -k 0
```

CI also builds GCC 15 Release, Clang 21 Release, and Clang 18 Debug; a
failure in a configuration not built here means building that one
locally.

## clang-tidy

Needs clang-tidy 19+ and the same-version Clang (it rejects GCC's
options), in its own build directory. It runs as part of the build. CI
([clang-tidy.yml](../../../.github/workflows/clang-tidy.yml)) uses
clang-tidy 23 and checks Debug and Release (`NDEBUG` and
`_FORTIFY_SOURCE` change the analysis); locally, Debug is enough:

```bash
cmake -B build-tidy -G Ninja -DCMAKE_C_COMPILER=clang-23 \
  -DCMAKE_BUILD_TYPE=Debug -DERNIC_BUILD_P4=ON -DERNIC_USE_CLANG_TIDY=ON \
  -DERNIC_CLANG_TIDY_EXE=/usr/bin/clang-tidy-23
cmake --build build-tidy -- -k 0
```

Every finding is an error. AGENTS.md says when a `NOLINTNEXTLINE` is
acceptable and what to do with `.clang-tidy`'s `TODO` list.

## Clang's thread-safety analysis

Every clang build, CI's clang 18 included, fails on a broken function
annotation. Only clang 23 checks fields' annotations and the lock order
in C. Build with it when a change touches a lock, a guarded field, or an
annotation;
[thread-safety.yml](../../../.github/workflows/thread-safety.yml) does
the same, failing only on these warnings:

```bash
cmake -B build-clang23 -G Ninja -DCMAKE_C_COMPILER=clang-23 \
  -DCMAKE_BUILD_TYPE=Debug -DERNIC_BUILD_P4=ON \
  -DCMAKE_C_FLAGS="-Werror=thread-safety -Werror=thread-safety-beta -Werror=thread-safety-negative -Werror=thread-safety-pointer"
cmake --build build-clang23 -- -k 0
```

## include-what-you-use

Run it when a change touches `#include`s. Install it with
`apt install iwyu` and build with the clang it's built on
(`include-what-you-use --version`), in its own build directory: IWYU
rejects options it doesn't know, such as GCC 15's
`-fzero-init-padding-bits`. It skips up-to-date files, so a rebuild
checks only what changed. To check every file as CI
([iwyu.yml](../../../.github/workflows/iwyu.yml)) does, start from an
empty build directory:

```bash
cmake -B build-iwyu -G Ninja -DCMAKE_C_COMPILER=clang-19 \
  -DCMAKE_BUILD_TYPE=Debug -DERNIC_BUILD_P4=ON -DERNIC_USE_IWYU=ON \
  -DERNIC_IWYU_EXE="$PWD/ci/iwyu/iwyu-wrapper.sh"
mkdir build-iwyu/iwyu-runs
ERNIC_IWYU_LOG_DIR="$PWD/build-iwyu/iwyu-runs" cmake --build build-iwyu
ci/iwyu/check-iwyu.sh build-iwyu/iwyu-runs
```

Omit `ERNIC_IWYU_EXE` to just see suggestions in the build output. CI
fails only if IWYU itself fails, and lists the files with suggestions.

Apply the correct suggestions for files you touch, such as adding a
header for a symbol you only get indirectly, or dropping `<stdint.h>`
where `<inttypes.h>` is included. Ignore the wrong ones:

- Forward-declaring a module's opaque type (e.g. `struct nvmeof_cm;`) in
  a file that includes the module's header.
- Naming an internal header instead of the public one, or insisting on
  one of several headers that repeat a typedef (legal in C11;
  `rocm_ernic_compat.h` and `rocm_ernic_internal.h` both do for
  `rocm_ernic_dev_t`). Fix these with a mapping in
  [`cmake/iwyu-libc.imp`](../../../cmake/iwyu-libc.imp) or
  [`cmake/iwyu-ernic.imp`](../../../cmake/iwyu-ernic.imp), or an
  `// IWYU pragma: export` in the header that should provide the symbol.

## Coverage

Line coverage of `src/` must not fall below `MIN_LINE_COVERAGE` in
[build-and-test.yml](../../../.github/workflows/build-and-test.yml),
which CI's coverage leg checks on every PR. Measure it when a change adds
error paths or a lot of code and `src/` is near the floor. Use Clang and
Debug, as CI does, so the lines match the source:

```bash
cmake -B build-coverage -G Ninja -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug -DERNIC_BUILD_P4=ON \
  -DERNIC_USE_CODE_COVERAGE=ON
cmake --build build-coverage
ctest --test-dir build-coverage -j"$(nproc)"
cmake --build build-coverage --target ernic-coverage
```

`ernic-coverage` writes `build-coverage/coverage/report.txt` (the
`TOTAL` line's tenth field is the line coverage CI compares) and
`coverage/lines.txt`, the coverage of each line. Profiles
accumulate across runs; delete `build-coverage/coverage/profraw` to
start over. CI also counts the loopback configurations
(`tests/test_loopback_ci.sh`), so its figure can be slightly higher.

## Static analysis

PRs labelled `run-static` also get clang-tidy, cppcheck, flawfinder, and
the Clang static analyzer, via `nix/analysis/`
([static-analysis.yml](../../../.github/workflows/static-analysis.yml)).
It never fails, but read its findings for files you touched. Its
clang-tidy checks differ from `.clang-tidy`, so it doesn't replace the
check above.
