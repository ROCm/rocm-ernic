<!--
Copyright (C) Advanced Micro Devices, Inc.
SPDX-License-Identifier: GPL-2.0-or-later
-->

# The two ionic bug fixes, in their mainline form

Two fixes for bugs of the upstream ionic Ethernet driver that the devmem
tests found. Neither is part of a devmem series, and both bugs are in
mainline. These are the patches against `v7.4-rc3`, as they would be
sent to `net`. Their `v7.2.6` forms are what guests get: `0003` and
`0004` of the base series in
[`ansible/files/patches`](../../../../ansible/files/patches), whose README
says what each bug does and when to drop the patch.

| Patch | Base series |
| --- | --- |
| `0001-ionic-use-the-rx-buffers-page-offset-for-xdp-frags.patch` | `0003-ionic-use-the-Rx-buffer-s-page-offset-for-XDP-frags.patch` |
| `0002-ionic-free-every-temporary-queue-after-a-queue-count-change.patch` | `0004-ionic-free-every-temporary-queue-after-a-queue-count.patch` |

The devmem Rx series in this repository
([`ansible/files/patches/devmem-rx`](../../../../ansible/files/patches/devmem-rx))
is based on `v7.4-rc3`, then `0001`, then the Tx series: its fourth
patch rewrites the lines `0001` fixes and does not apply without it, as
its README says. `tools/patch-series/rx-mainline/verify.sh` builds that
base.
`tools/patch-series/xdp-frag-offset/` holds the script and the message
`0001` was made from.

Neither carries a `Signed-off-by:`. They are `GPL-2.0`, like the files
they change.

**Tested:** `0001` on `v7.4-rc3` with KASAN, in two guests of this
emulator: of 10,000 8000-byte datagrams echoed by an `xdp.frags` program
at MTU 9000, 5000 were corrupt before it and none after
(`test-tx-7.4-rc3.md`, finding B1). `0002`: the leak was reproduced on the unmodified
`v7.4-rc3` driver, and a module with the fix was run through the same
sequence (`test-rx.md`, finding P1, in
[`docs/devmem/notes`](../../../../docs/devmem/notes)). The `v7.2.6`
forms were run separately, each bug reproduced without its fix and gone
with it (`test-ionic-fixes-7.2.6.md`).
