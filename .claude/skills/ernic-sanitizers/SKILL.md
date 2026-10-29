---
name: ernic-sanitizers
description: "Build and run rocm-ernic's tests under AddressSanitizer (with LSan and UBSan) or ThreadSanitizer, and loop threaded tests to shake out races. Use for a change to pointers, buffers, lengths, or input from outside the process (ASan), or to shared state, locks, or threads (TSan); when a sanitizer CI leg fails; when a test is flaky; or when a TSan run aborts with 'unexpected memory mapping'."
---

# Sanitizers

The rules (every report is fatal; flags only in `ErnicSanitizers.cmake`;
thread-safety requirements) are in [AGENTS.md](../../../AGENTS.md); this
file says how to run them. CI runs ASan and TSan with GCC and Clang,
Debug and Release; locally, one compiler and Debug is enough, running
only the tests that exercise the change (`-R`).

`ERNIC_USE_SANITIZERS` (ASan, LSan, UBSan, and compatible checks; see
[`ErnicSanitizers.cmake`](../../../cmake/ErnicSanitizers.cmake)) and
`ERNIC_USE_THREAD_SANITIZER` can't be combined, so each gets its own
build directory. The default run-time options, including TSan's and
UBSan's `halt_on_error=1`, are in
[`sanitizer-default-options.c`](../../../cmake/sanitizer-default-options.c);
`*SAN_OPTIONS` override them.

## ASan

```bash
cmake -B build-asan -G Ninja -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug -DERNIC_BUILD_P4=ON -DERNIC_USE_SANITIZERS=ON
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=1 \
  ctest --test-dir build-asan --output-on-failure -R '<tests>'
```

Under full ASan, the tests that start the server can time out:
`nvmeof-ci`, `s3-ci`, `ionic-ci`, `ionic-server-smoke`,
`pci-config-test`. That isn't evidence against a change; a real failure,
or a timeout without sanitizers, is.

## TSan

Build with `-g -Og`, as CI does: `-O0` hides races. Run the tests under
`setarch "$(uname -m)" -R`, as CI does. Without it, most abort with
`FATAL: ThreadSanitizer: unexpected memory mapping`, which is ASLR
colliding with TSan's shadow memory, not a test failure.

```bash
cmake -B build-tsan -G Ninja -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-g -Og" \
  -DERNIC_BUILD_P4=ON -DERNIC_USE_THREAD_SANITIZER=ON
cmake --build build-tsan
setarch "$(uname -m)" -R \
  ctest --test-dir build-tsan --output-on-failure -R '<tests>'
grep -n 'WARNING: ThreadSanitizer' build-tsan/Testing/Temporary/LastTest.log
```

A passing test doesn't prove TSan was quiet: not every script fails on a
report from the server it started, hence the `grep`.

TSan can't see the guest, which writes guest-shared memory (rings,
indices) from another process; atomics and fences there are chosen by
reasoning about the guest driver, not by TSan.

## Looping threaded tests

A new or changed threaded test must be looped in a way that moves the
timing: hundreds of runs in one process, pinned to one CPU with
`taskset -c 0`. Pinning matters: one race failed 13 of 300 runs
unpinned and 168 of 300 pinned.

To confirm that a test covers a window, add a delay there in a scratch
copy of the source and check that the test fails.

A flaky test is a suspected race. Find the race; don't retry the test,
lengthen its timeout, or add a sleep.
