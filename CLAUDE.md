# Notes for Claude

## Running the sanitizers

ASAN/UBSAN/LSAN and TSAN are separate, mutually exclusive builds —
configuring both options together is a hard CMake error.

```sh
# ASAN + LSAN + UBSAN
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DERNIC_USE_SANITIZERS=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure

# TSAN
cmake -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DERNIC_USE_THREAD_SANITIZER=ON
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

No manual wrapper is needed: under `ERNIC_USE_THREAD_SANITIZER` the build
prefixes every registered test with `setarch <arch> -R`, via
`ERNIC_TEST_LAUNCHER` in `cmake/ErnicSanitizers.cmake`. The launcher is empty
in every other configuration.

That workaround exists because TSan maps a large fixed shadow region at
startup and aborts with `FATAL: ThreadSanitizer: unexpected memory mapping`
when the loader has already randomised something into it. Ubuntu's 32 bits of
mmap entropy makes that happen on nearly every run, so **an unwrapped TSAN run
fails almost every test before `main()` and looks like a broken test suite.**

If you ever see that `FATAL:` line, the ASLR workaround is not being applied —
check whether `setarch` is installed and whether `ERNIC_TEST_DISABLE_ASLR` was
turned off, rather than investigating the tests. `sudo sysctl
vm.mmap_rnd_bits=28` is the equivalent fix for anyone who prefers it; configure
with `-DERNIC_TEST_DISABLE_ASLR=OFF` in that case.

A passing TSAN run is not sufficient on its own: a test can pass while TSan
reports a race, depending on exit-code handling. Grep the output for
`WARNING: ThreadSanitizer` / `SUMMARY: ThreadSanitizer` as well.

Per the global instructions: build and test with ASAN for pointer changes,
and with TSAN for concurrency changes.

## Copyright lines

AMD copyright lines do not carry a year. Write:

```
Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
```

not `Copyright (c) 2026 Advanced Micro Devices, Inc.`. Do not add a year to a
new file, and do not "update" a year on an existing one.

Two forms are in use, split by license:

- MIT-licensed files (everything outside `src/from-qemu/`) use the full line
  above, matching `LICENSE.md`.
- GPL-licensed files carried over from QEMU (`src/from-qemu/`) use
  `Copyright (C) Advanced Micro Devices, Inc.` — capital `(C)`, no
  "All rights reserved" — and keep the QEMU GPL boilerplate that follows it.

Copy the header from a neighbouring file in the same directory rather than
composing one from scratch.

Third-party copyright lines are left exactly as they arrived, years included.
