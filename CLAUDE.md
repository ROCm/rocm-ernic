# Notes for Claude

## C code

C code is standard C11. Do not use GNU extensions without permission.
Assume that atomics and multithreading are available.

Code must compile warning-free under both clang and gcc.

Code must pass sanitizer checks (ASAN, LSAN, TSAN, etc.).

Code must be formatted using the version of clang-format we use
in the CI.

## Other code

Python 3.10 is usually the minimum supported Python standard.

bash scripts must be shellcheck-clean.

## Security

Be very careful of integer security issues. Do not blindly cast to
clear warnings. Prefer explicitly sized integer types instead of
int, long, etc.

Any data intended for the wire MUST be cleared before use.

## Warning suppression

Use (and extend) the warning suppression macros in
`shared/rocm-ernic-warnings.h` to suppress warnings. Only suppress
warnings that are almost impossible to fix due to things like
outside or vendored code. If you remove a warning suppression, check
to see if it is referenced elsewhere in the code to avoid creating
orphaned macros.

## Vendored code

Code in third-party does not belong to AMD and should not be modified,
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
