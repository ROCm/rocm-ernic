<!--
Copyright (C) Advanced Micro Devices, Inc.
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Fault-injection knobs for the ionic Rx series (test only)

`test-only-ionic-fault-knobs.diff` adds module parameters to ionic,
under `/sys/module/ionic/parameters/`, that make paths run which the
emulated device never causes by itself. **Never ship a module built with
it**: any root user can then fail the driver's queue operations or put
it through a firmware reset.

It applies on top of the devmem Rx series
([`ansible/files/patches/devmem-rx`](../../../../ansible/files/patches/devmem-rx),
on `v7.4-rc3`), as a plain `patch -p1` diff, not a `git am` mbox.

| Parameter | Effect |
| --- | --- |
| `fi_qstart_fail` | fail the next N `ionic_queue_start()` calls |
| `fi_qop_fw_reset` | begin a firmware reset in the next N `ionic_queue_stop()` calls |
| `fi_link_down` | the link check sees the link down |
| `fi_rx_alloc_fail` | fail Rx buffer allocations |
| `fi_cmd` | write-only; `fwdown`, `fwdown-hold` and `fwup` run the driver's firmware down and up handling, `txtimeout` schedules the Tx timeout work, `linkcheck` requests a link check |

The emulated firmware stays alive throughout: these reach the driver's
handlers, not a device that has really stopped.

`scripts/stack2/rv-fi.sh` and `scripts/stack2/guest/rv-refill.sh` drive
them; the results are in the "Review round" of `test-rx.md`, in
[`docs/devmem/notes`](../../../../docs/devmem/notes).

The diff changes `GPL-2.0` ionic files and is `GPL-2.0`.
