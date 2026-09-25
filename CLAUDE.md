# Notes for Claude

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

Copy the header from a neighbouring file in the same directory rather than
composing one from scratch.

Third-party copyright lines are left exactly as they arrived, years included.
