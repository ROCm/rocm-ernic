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
automation code: the `CMakeLists.txt` files, `flake.nix`, `cmake/`,
`docs/`, `ansible/` apart from the patch series it carries in
`ansible/files/patches/` and `ansible/playbooks/files/rdma-tutorial/`,
`ci/`, `.github/`, `prometheus/`, `udev/`, `scripts/`, `service/`,
`tools/`, and `nix/` apart from the fuzz harnesses
below. They also cover the headers in `shared/`, which carry their own
third-party notices where they were derived from other projects, and the
READMEs in `ansible/files/patches/`.

The emulator itself is `GPL-2.0-or-later` (see `LICENSE_GPL.md`):

* Everything under `src/` and `tests/`, and the fuzz harnesses in
  `nix/analysis/fuzz/`, which build against `src/`.

* The files under `src/pvrdma/` were imported from the QEMU project
  (`https://gitlab.com/qemu-project/qemu`). The VMware/Linux uAPI header
  `src/pvrdma/vmw_pvrdma-abi.h` is instead dual `GPL-2.0` / `BSD-2-Clause`,
  as stated in its header comment.

The patches under `ansible/files/patches/` modify code from other projects
and are licensed under the terms of the files they modify:

* `ansible/files/patches/srq/` and the patch at the top of
  `ansible/files/patches/` modify the Linux
  ionic drivers. The RDMA driver in `drivers/infiniband/hw/ionic/` is
  `GPL-2.0`, the uAPI header `include/uapi/rdma/ionic-abi.h` is
  `GPL-2.0 WITH Linux-syscall-note`, and the Ethernet driver's
  `drivers/net/ethernet/pensando/ionic/ionic_if.h` is
  `(GPL-2.0 OR Linux-OpenIB) OR BSD-2-Clause`.

* `ansible/files/patches/rdma-core/` modifies rdma-core's ionic provider in
  `providers/ionic/`, which is `GPL-2.0 OR Linux-OpenIB`, and rdma-core's
  copy of the uAPI header in `kernel-headers/rdma/ionic-abi.h`, which is
  `GPL-2.0 WITH Linux-syscall-note`.

The patches under `ansible/playbooks/files/rdma-tutorial/` modify the
examples of [RDMA-Tutorial](https://github.com/jcxue/RDMA-Tutorial), which
is `Apache-2.0`, and are `Apache-2.0` too. Each one says at its head which
upstream commit it applies to and what it changes.

`ansible/files/scripts/setup-ionic-dkms.sh` is MIT, but the `dkms.conf` it
writes for the patched ionic drivers is `GPL-2.0`, like the drivers it
builds. The
`GPL-2.0` tag inside the script belongs to that generated file.

The groupings above are a summary; the per-file notice is authoritative.
Every `.c` and `.h` file carries an `SPDX-License-Identifier` tag. Some
other files -- build fragments, dotfiles, documentation, data -- carry no
notice at all and take the license of the directory they sit in. The
patches under `ansible/files/patches/` carry no notice of their own; the
SPDX tag on each file they modify, in its own project, is authoritative
for them.
