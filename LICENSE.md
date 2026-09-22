MIT License

Copyright (c) Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

---

## Files with different licenses

The MIT terms above cover the build system and the deployment and
automation code: the `CMakeLists.txt` files, `cmake/`, `docs/`, `ansible/`,
`ci/`, `.github/`, `prometheus/`, `udev/`, `scripts/`, and `service/`.

The emulator itself is `GPL-2.0-or-later` (see `LICENSE_GPL.md`):

* Everything under `src/` and `tests/`, and the fuzz harnesses in
  `nix/analysis/fuzz/`, which build against `src/`. The one exception is
  `tests/test_write_imm.c`, which is MIT and carries a
  `Copyright (c) Gluesys Inc. and Jihyeon Gim` notice alongside AMD's.

* Many of the files under `src/` were imported from the QEMU project
  (`https://gitlab.com/qemu-project/qemu`). The VMware/Linux uAPI headers
  under `src/from-qemu/include/qemu-extra/standard-headers/` are instead
  dual `GPL-2.0` / `BSD-2-Clause`, as stated in each file's header comment.

The groupings above are a summary; the per-file notice is authoritative.
Every `.c` and `.h` file carries an `SPDX-License-Identifier` tag. Some
other files -- build fragments, dotfiles, documentation, data -- carry no
notice at all and take the license of the directory they sit in.
