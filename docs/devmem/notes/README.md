<!--
Copyright (c) Advanced Micro Devices, Inc. All rights reserved.

SPDX-License-Identifier: MIT
-->

# devmem TCP proof of concept: working notes

The notes written while devmem TCP was brought up on the emulated ionic
NIC: source studies, design, implementation reports, test reports and
runbooks. They are the record behind
[`docs/devmem.rst`](../../devmem.rst), which is the page to read first.

**They are working notes, not documentation.** Read them knowing that:

- **Later sections correct earlier ones.** Many notes end in a
  "Verification pass", "Review round" or dated "CORRECTION" section,
  written after someone checked the text above it. Where the two
  disagree, the later section is right, and the text above was left as
  it was. Read a note's last sections before relying on its first.
- **They are a snapshot.** Each describes the tree, the kernels and the
  test stacks on the day it was written (2026-11-18 to 2026-11-20).
  Statements such as "the stack is left running" were true then.
- **Paths are those of the development host.** `/var/tmp/ionic-devmem`
  was a scratch work area there, and most of what the notes point at
  was not kept: the kernel trees, the guest images, the built modules,
  and above all the evidence (2.2 GB of logs and captures under
  `test/evidence`, `test-rx/evidence` and `test/evidence-emu-rx`). A
  citation of an evidence file names something this repository does not
  have. What was kept is listed below.
- **They were edited on the way in, and only so**: home directory
  paths, a login name (it remains in the names of images and
  registries), the name of an access token, a guest image's password and
  the host's LAN were removed; trailing spaces were stripped from two
  lines; five words were respelt for the spelling check; and in three
  notes a few citations had the directory
  `drivers/net/ethernet/pensando/ionic/` or
  `tools/testing/selftests/drivers/net/hw/` shortened with `...`, and
  one image digest was shortened, because a secret scanner took the long
  strings for keys. Nothing else was changed, so they still say "I",
  name the person the work was for, and contradict each other where a
  correction says so.
- The emulator changes they describe as uncommitted, or as living in
  `rocm-ernic-rx`, are in this repository now: the Tx offloads and the
  `header-split` profile are documented in `docs/ionic.rst`.

Sphinx does not build these files: they are Markdown, and `docs/conf.py`
excludes the directory.

## Where the paths in the notes are now

| In the notes (under `/var/tmp/ionic-devmem`, or `T` = `test`) | In this repository |
| --- | --- |
| `notes/` | this directory |
| `patches/mainline/` | `ansible/files/patches/devmem-tx-mainline/` |
| `patches/ernic-v7.2.6/` | `ansible/files/patches/devmem-tx/` |
| `patches-rx/mainline/` | `ansible/files/patches/devmem-rx/` |
| `patches/xdp-frag-offset/net/`, `patches-rx/extra/0001-*.patch` | `tests/devmem/kernel-patches/ionic-mainline-fixes/`; as `v7.2.6` patches, `ansible/files/patches/0003-*` and `0004-*` |
| `patches-rx/extra/test-only-ionic-fault-knobs.diff` | `tests/devmem/kernel-patches/ionic-test-knobs/` |
| `patches/core-unbind-uaf/` | `tests/devmem/kernel-patches/net-core-v7.2.15/` |
| `amdgpu-31.70/` (`README.md`, `patches/`, `scripts/build.sh`) | `tests/devmem/kernel-patches/amdgpu-dkms-31.70-v7.4/` |
| `test/gpu/patches/` | `tests/devmem/kernel-patches/amdgpu-intree-v7.4/` |
| `test/src/`, `test/evidence/tx-verify/src/*.c`, `vrx.py`, `test-rx/guest/*.py` | `tests/devmem/src/` |
| `*.bt` (`test/scripts/devmem-tx/probe.bt` is `tx-probe.bt`) | `tests/devmem/probes/` |
| `test/scripts/` | `tests/devmem/scripts/stack1/` |
| `test-rx/scripts/`, `test-rx/guest/*.sh` | `tests/devmem/scripts/stack2/`, `stack2/guest/` |
| `test/evidence/tx-verify/src/*.sh`, `vsum.py` | `tests/devmem/scripts/tx-verify/` |
| `test/scripts/drun.sh`, `test/builder/Dockerfile` | `tests/devmem/scripts/host/` |
| `test/stack/devmem-fleet.xml`, `test-rx/stack/rx-fleet.xml` | `tests/devmem/scripts/stack1/`, `stack2/` |
| `patches/scripts/`, `patches-rx/scripts/`, `backport-scratch/scripts/` | `tests/devmem/tools/patch-series/` |
| `test/emu-golden*/`, `test/emu-mutate/`, `test/emu-rx-mutate/` | `tests/devmem/tools/` |
| `test/evidence/tx-verify/SUMMARY-*.txt` | `tx-verify-summary-*.md` here |
| `rocm-ernic-pin`, `rocm-ernic-rx`, `test/ernic/*/src` | snapshots of this repository; the work is on its branch now |
| `linux`, `linux-rx`, `k74`, `k726`, `linux-7.2.*`, `test/kbuild`, `test/images`, `test/ionic-mod`, every `evidence` directory, `qemu-minimal*`, `kernel-tools`, `test-rocm102` | not kept |

`tests/devmem/scripts/work-area.map` has the same mapping file by file,
and `tests/devmem/scripts/deploy.sh` recreates the scripts' places in a
work area. `dmesg-lines.py` is `dmesg_lines.py` now, and `mutate_asan.py`
is a mode of `mutate.py`.

The citation prefixes the notes define at their heads (`E:`, `K74:`,
`K726:`, `Q:` and so on) name source trees at a commit: this repository,
Linux at `v7.4-rc3`, `v7.2.6` or `v7.2.15`, QEMU, libvfio-user,
qemu-minimal.

## The notes

Studies, written before any code (each has a "Verification pass" that
corrects it):

| Note | What it is |
| --- | --- |
| `kernel-core-devmem-tx.md` | What the networking core needs from a driver for devmem Tx, on `v7.4-rc3`, `v7.2.15` and `v7.2.6` |
| `reference-drivers.md` | How the upstream drivers with netmem Tx do it, and a reviewer's checklist for the ionic series |
| `ionic-tx-audit.md` | The ionic Tx path audited for unreadable skbs |
| `ionic-tx-device-contract.md` | What an ionic device does with Tx descriptors: the contract the emulator had to meet |
| `ernic-emu-datapath.md` | The emulator's Ethernet datapath as it was before the Tx offloads. **Its statement that the server cannot read a GPU BAR is superseded** by `test-gpu-vram.md`: the fork QEMU passes the region's file descriptor. The note has no CORRECTION of its own |
| `selftest-ncdevmem.md` | The kernel's `ncdevmem` and `devmem.py` as test drivers |
| `ernic-vm-stack.md` | How to stand up two guests, before a test bed existed. Superseded as a runbook by `testbed.md` |
| `ernic-gpu-dmabuf.md` | Whether an amdgpu dma-buf could be the Tx source. **Its conclusion that the emulator cannot read GPU memory is retracted** by its own CORRECTION and by `test-gpu-vram.md` |
| `rx-design.md` | Design study for devmem Rx through a header-split mode. Where it differs from `rx-interface.md`, that file wins |

Design and implementation:

| Note | What it is |
| --- | --- |
| `impl-emulator.md` | The emulator's Tx scatter-gather, checksum and TSO: what was built and how it was checked |
| `impl-driver.md` | The mainline Tx series. The series' cover letter still says "build only"; the test reports below are later |
| `impl-backport.md` | The backport of that series to `v7.2.6` |
| `rx-interface.md` | **The device interface of the `header-split` mode**, as the emulator and the Rx series implement it. Invented for the proof of concept; no ionic firmware has it |
| `impl-emulator-rx.md` | The emulator's header-split mode |
| `impl-driver-rx.md` | The Rx driver series, with two review rounds |

Test reports:

| Note | What it is |
| --- | --- |
| `test-emulator-offloads.md` | The Tx offloads against unpatched ionic drivers on three kernels |
| `test-tx-7.2.15.md` | The devmem Tx matrix, backport series, `v7.2.15` |
| `test-tx-7.4-rc3.md` | The same matrix, mainline series, `v7.4-rc3` with KASAN and lockdep; the two bugs it found |
| `test-tx-verification.md` | An independent re-run of both with its own tools. Corrects both reports on the frag limit |
| `tx-verify-summary-7.2.15.md`, `tx-verify-summary-7.4-rc3.md` | That pass, one line per case |
| `test-gpu-dmabuf.md` | A GPU dma-buf as the Tx source on `v7.2.x`: it bound, and the data came from system RAM. Two statements are retracted by its CORRECTION |
| `test-gpu-vram.md` | The run in which the NIC read GPU VRAM, on `v7.4-rc3` |
| `test-rx.md` | devmem Rx on the header-split mode, and the review round that followed |
| `rx-xdp-frag-fix.md` | The XDP frag offset bug inside the Rx series: the series regenerated on top of the fix, and the before and after runs. The series it describes is the one in `ansible/files/patches/devmem-rx`; its section 8 is the hand-over that was followed |
| `test-ionic-fixes-7.2.6.md` | The two bug fixes of the base series, run on the `v7.2.6` sources the guests build (as an external module on a `v7.2.15` kernel, not through DKMS): each bug reproduced without its fix and gone with it |

Runbooks and the third stack:

| Note | What it is |
| --- | --- |
| `testbed.md` | Stack 1, the Tx test bed. Sections 10 to 14 were added by later runs and override the earlier sections; it says to read 12 and 11 first |
| `real-hardware-runbook.md` | How the Tx tests would be run on real ionic and GPU hardware. **Not run.** Its CORRECTION updates what it says the emulator cannot do |
| `qemu-tool-2.1-rocm102.md` | What qemu-minimal's `qemu-tool` v2.1.0 and its ROCm 10.2 guest image provide |
| `rocm102-baseline.md` | Stack 3 on that image, before any change |
| `rocm102-devmem.md` | The server of this branch and the devmem Tx series on stack 3 |
| `rocm102-gpu.md` | GPU buffers as the Tx source on stack 3: system memory on the stock kernel, VRAM with a hand backport of core commit `69ab87acc614`. The backport's diff was in stack 3's directory and is not in this repository |
| `rocm102-verification.md` | An independent check of the three stack 3 reports. Read it with them: it refutes no pass, marks four claims as overstated, and its F1 says what the frag limit does to a connection |

Other work went on while these files were copied, on the evening of
2026-11-19 and the night after. Every note was compared with the work
area again late that evening and was the same apart from the edits
listed above. **The stack 3 notes may still be incomplete**
(`qemu-tool-2.1-rocm102.md` and the four `rocm102-*.md`): work on that
stack was still running, so they may be behind their authors' last
version. `rx-xdp-frag-fix.md` and `test-ionic-fixes-7.2.6.md` were
finished; they, and the last sections of `test-rx.md`,
`impl-driver-rx.md` and `testbed.md`, point at an `overnight` directory
of the work area that, like the rest of it, was not kept.
