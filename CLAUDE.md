# Notes for Claude

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
