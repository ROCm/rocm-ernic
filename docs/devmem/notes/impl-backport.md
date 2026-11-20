# ionic devmem TX: backport to the ERNIC DKMS sources (v7.2.6)

Written 2026-11-18. Nothing was committed, pushed or posted anywhere.
The earlier, interrupted run of this task had left nothing for it
(`patches/ernic-v7.2.6/` was empty, no `backport-scratch/`, no
`build-7215/`), so everything below is from this run.

## Result

`/var/tmp/ionic-devmem/patches/ernic-v7.2.6/` holds seven hand-built
mbox files, copied byte for byte to
`rocm-ernic/ansible/files/patches/devmem-tx/` (uncommitted):

| # | File | What | sha256 (first 16) |
| --- | --- | --- | --- |
| 0001 | `0001-ionic-use-netif_txq_maybe_stop-in-ionic_tx.patch` | upstream c91b4d6e5cc3 = v7.2.15's e247d033dd32, unchanged | 4179b2051fe1731b |
| 0002 | `0002-ionic-dont-leave-a-failed-xdp-frame-in-the-tx-descriptor-info.patch` | mainline 1/6, diff unchanged | 024c8959c850bd7a |
| 0003 | `0003-ionic-give-tx-buffers-their-own-bookkeeping-struct.patch` | mainline 2/6, diff unchanged | d65ad739cbb6ed04 |
| 0004 | `0004-ionic-unmap-tx-frags-with-the-netmem-helpers.patch` | mainline 3/6, diff unchanged | 43c6675a5eb356af |
| 0005 | `0005-ionic-count-the-unreadable-skbs-dropped-on-tx.patch` | mainline 4/6 without the `Documentation/` hunk | 59220453747aaf05 |
| 0006 | `0006-ionic-dont-send-an-unreadable-skb-that-has-no-frags.patch` | mainline 5/6, diff unchanged | faff20404235cd63 |
| 0007 | `0007-ionic-add-netmem-tx-support.patch` | mainline 6/6, line numbers only (-3) | 6cd68a213a436df8 |

They were generated from the mainline series as it was at 18:56
(sha256 prefixes 30fcfdde, d67a5856, 8dc7ed9d, f755e6b2, d2232eb9,
9c7a9a50 for 0001..0006). **If the mainline series is revised, rerun
`backport-scratch/scripts/mkstages.sh` (after moving `stages/` aside),
`mkpatches.sh`, `verify.sh` and copy again.**

Each message is the mainline message plus a closing
`[ rocm-ernic: ... ]` paragraph naming the upstream-series patch and the
difference. Subject tag is `[PATCH]`, as the srq and p4 patches have.
No `Signed-off-by: Stephen Bates` anywhere. **0001 keeps the trailers of
the stable commit it is** (Signed-off-by of Nikhil Rao, Jakub Kicinski,
Sasha Levin; Reviewed-by Brett Creeley), as srq/0001-0003 keep theirs:
removing them would misstate the upstream commit. Delete those lines if
"no Signed-off-by" was meant literally for every file.

## What the DKMS source really is, and the v7.2.6..v7.2.15 change

- The DKMS source is v7.2.6 (`IONIC_KERNEL_REF`), three directories,
  compiled against the running kernel's headers (v7.2.15 in the bundle).
- Between v7.2.6 and v7.2.15, in those three directories, exactly one
  ionic commit: e247d033dd32 "ionic: use netif_txq_maybe_stop() in
  ionic_tx()" (upstream c91b4d6e5cc3), in `ionic_txrx.c`. (The only
  other change is in `include/uapi/rdma/bnxt_re-abi.h`, not ionic.)
- Confirmed: `ionic_txrx.c` at v7.2.15 == v7.4-rc3; `ionic_dev.h` and
  `ionic_stats.c` are identical at v7.2.6, v7.2.15 and v7.4-rc3;
  `ionic_lif.c` and `ionic_lif.h` differ from v7.4-rc3 (PHC state page
  etc.) but not near the series' hunks. srq and p4 touch only
  `ionic_lif.h` among the five files, elsewhere in it.
- **Decision: carry it, as 0001.** It is a fix in its own right (a queue
  stop without `netif_txq_maybe_stop()`'s barriers, Fixes: 138506ab249b),
  it is what the kernel the module runs on ships, and with it
  `ionic_txrx.c` is byte-identical to the file the mainline series was
  written against, so five of six patches apply as they are. Cost: it
  changes the Tx path of every guest built with this directory, devmem
  or not. Without it 0002-0007 still apply with `git apply`
  (tried in `backport-scratch/no0001`); that tree was not built.
- Mainline 5/6 ("not for net-next") stays: v7.2.15's
  `skb_copy_and_csum_bits()` zero-fills for an unreadable skb and returns
  0, exactly as v7.4-rc3's (read in both trees).
- v7.2.15 and v7.2.6 both have `NETMEM_TX_DMA`,
  `netmem_dma_unmap_addr_set()`, `netmem_dma_unmap_page_attrs()`
  (grep of the headers; only v7.2.15 was built against).

## What was run and seen

Scratch: `/var/tmp/ionic-devmem/backport-scratch/` (scripts in
`scripts/`, logs beside them). No git repository there; nothing
committed.

1. `mkbase.sh`: copy of the three directories from `linux-7.2.6`, then
   srq 0001-0009 and p4 0001 from rocm-ernic (feat/devmem-poc) with
   `git apply --whitespace=fix`: all applied. Result identical
   (`diff -rq`) to the test-bed track's `test/ionic-mod/ernic-base/tree`.
2. `mkstages.sh`: stage b1 = + stable diff, b2..b7 = + mainline 1..6 with
   `git apply --exclude='Documentation/*'`. All clean; one offset (-3)
   in `ionic_lif.c`. b1 `ionic_txrx.c` == v7.2.15's; b7 `ionic_txrx.c`,
   `ionic_dev.h`, `ionic_stats.c` == the patched v7.4-rc3 working tree.
3. `verify.sh` (log `verify.log`): on a fresh tree, each of the seven
   files: `git apply --check` rc=0, `git apply --whitespace=error-all`
   rc=0, result identical to its stage; `git mailinfo` rc=0 with the
   right author/subject; the diff mailinfo extracts reverse-applies to
   the tree; `git mailsplit` finds 1 message per file. **`git am` was
   not run** (it commits).
4. `combos.sh` (log `combos.log`): the series also applies
   (`--check` + apply) to plain v7.2.6, to base (top-level 0002), to
   base+p4, and to srq alone; the four non-`ionic_lif.h` files come out
   identical in all of them.
5. The workflow's subject check (its sed expression) was run by hand
   against the seven files: every subject equals what mailinfo yields.
6. Build (logs `build-*.log`, summary `build-steps.log`): in the
   `devmem-kbuild:1` container (gcc 15.2.0, the compiler that built the
   kernel), `make -C test/kbuild/7.2.15/out M=<staged tree> W=1 modules`
   against the test-bed track's **built v7.2.15-devmem tree**
   (`NET_DEVMEM=y`, `UDMABUF=y`, `PAGE_POOL=y`, `DMA_API_DEBUG=y`,
   `NEED_DMA_MAP_STATE=y`, `INFINIBAND=m`). The staged tree is the
   layout `setup-ionic-dkms.sh` makes (`eth/`, `rdma/`,
   `uapi/rdma/ionic-abi.h`) with its Kbuild extracted verbatim from the
   script's heredoc. Results: base (srq+p4 only), after each of the
   seven patches, and the tree produced from the patch files:
   **rc=0, 24 CC, 0 warnings, 0 errors every time**; `ionic.ko` and
   `ionic_rdma.ko` produced, vermagic `7.2.15-devmem`.
   `/var/tmp/ionic-devmem/build-7215` was therefore not created.

Not run: `git am`; `fetch-ionic-sources.sh` end to end (network +
commits); `dkms build`; sparse; a build without
`CONFIG_NEED_DMA_MAP_STATE` (the mainline track did that for the same
code); a build against v7.2.6 headers; loading the module; any traffic.
The test kernel's source has the bundle's dmabuf backport patches on top
of v7.2.15 (its `net/core/skbuff.c`, `include/net/netmem.h` and ionic
directory are identical to plain v7.2.15's).

## Where it lives in rocm-ernic, and what was changed there

All uncommitted, in `~/Projects/rocm-ernic`
(feat/devmem-poc). Other tracks' edits in the same tree (emulator,
`CHANGELOG.md`, `docs/ionic.rst`) were left alone.

Added / changed by this task:

- `ansible/files/patches/devmem-tx/` (new): the seven patches +
  `README.md` (base, table, differences, how to apply, what was tested).
- `ansible/changelogs/fragments/ionic-devmem-tx-series.yml` (new).
- `CHANGELOG.md`: one entry under UNRELEASED / Added.
- `ansible/files/README.md`: one line in the tree listing.
- `docs/p4.rst`: "three series" -> four, with a bullet.
- `.github/workflows/ernic-driver-build.yml`: `devmem-tx` matrix leg of
  `ionic-patches` (on the base series, like p4), its ref case
  (`p4|devmem-tx)`), and a step checking that the tree sets `netmem_tx`
  and no longer calls `dma_unmap_page()` in `ionic_txrx.c`. This is the
  only place `git am` of the series will run. It could not be run here.

Checks run on those: codespell (clean), doc8 on `docs/p4.rst` (clean),
actionlint with ShellCheck 0.11.0 (clean), zizmor (no findings),
ansible-lint 26.10.1 `--strict` on the fragment (passed). Not run: the
Sphinx docs build, `ansible-test sanity`, `tools/check-embedded-shell.py`.

Deliberately NOT changed (the series is optional and unrun, so no
default was switched). To include it, each needs about one line:

| Caller | Where | Change |
| --- | --- | --- |
| CMake `fetch-ionic-sources` | `cmake/ErnicKernelModule.cmake:79` | add `--patches-dir "${IONIC_PATCHES_DIR}/devmem-tx"` after the existing one |
| in-VM installer | `service/vm-driver-install.sh.in:75` | add `--patches-dir "${SCRIPT_DIR}/patches/devmem-tx"` (`ErnicInstall.cmake:371` already installs the whole `patches/` tree) |
| `ernic_guest_setup` | `ansible/roles/ernic_guest_setup/defaults/main.yml:77` | `ernic_guest_ionic_extra_patches: [patches/devmem-tx]`, or per play as `ernic-p4-lane.yml:547` does for p4; no code change, the whole `files/patches` is staged |
| `ernic_p4_dkms` | `ansible/roles/ernic_p4_dkms/defaults/main.yml:24-26` | third entry `.../ansible/files/patches/devmem-tx` (needed when the guest's ionic has it: 0005 changes structs in `ionic_lif.h`, so symbol CRCs can differ); `ansible/playbooks/p4-tests.yml:34,57-70` would also have to stage the directory |
| `ernic_srq_dkms` | `ansible/roles/ernic_srq_dkms/tasks/build_one.yml:61` | takes one directory today; needs a second `--patches-dir` (a new variable) |
| ERNIC bundle (kernel-tools, untouched) | `ktool/ktool/build_ernic_bundle.py:294-305` | `devmem_dir = .../patches/devmem-tx`; `if devmem_dir.is_dir(): fetch_cmd += ["--patches-dir", str(devmem_dir)]` after the p4 one; needs `ernic_source_pin` moved to a commit that has the directory |
| qemu-minimal golden image (untouched) | `ansible/playbooks/vm-rocm-golden.yml:411-430` | its combined srq+p4 directory would copy these renumbered 0011-0017 |
| collection build assertion (optional) | `.github/workflows/ernic-ansible-collection.yml:236-240` | `require 'files/patches/devmem-tx/README.md'`, `count 'files/patches/devmem-tx/.*\.patch' 7` |
| test bed | `test/scripts/ionic-mod.sh stage NAME ernic-7.2.6 <dir>` | already takes a series directory: pass `patches/ernic-v7.2.6` |

## Open points

- 0001 changes Tx for every guest built with the directory; if that is
  unwanted in a PoC, it can be dropped: the other hunks don't overlap
  it and 0002-0007 apply without it (tried), but then `ionic_txrx.c`
  is no longer the reviewed file. Not built.
- The guest only gets `netmem_tx` from a server that grants TX_SG and
  TX_CSUM (the emulator work on this branch); with develop's server the
  bind fails with EOPNOTSUPP.
- The CHANGELOG/fragment/README say "built, not run". Update them once
  the test-bed track has run it.
