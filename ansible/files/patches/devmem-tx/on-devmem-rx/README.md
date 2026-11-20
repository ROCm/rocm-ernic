# The frag-limit fix, for a tree that also has a devmem Rx series

The same change as [`../0008-ionic-send-an-unreadable-skb-with-too-many-frags-as-a-tso.patch`](..),
regenerated for a tree that has one of the devmem Rx series applied as
well. Nothing here is a second fix: the added and removed lines are
identical to the ones `../0008` adds and removes, as are the mainline
form's in [`../../devmem-tx-mainline`](../../devmem-tx-mainline). Only
the context and the line numbers differ.

**A subdirectory is not a series.**
[`fetch-ionic-sources.sh`](../../../scripts/fetch-ionic-sources.sh)
applies `"${d}"/*.patch` for each `--patches-dir d`, so it never
descends: this directory is invisible to it, to
`ernic_guest_ionic_extra_patches`, to `ernic_p4_dkms_patches_dirs` and
to every CI job. Give it as a `--patches-dir` of its own, last, when you
want it.

## Why the Rx series needs its own form

The Rx series adds `rx_hds_split`, `rx_hds_hbo` and
`rx_unreadable_drop` to `struct ionic_lif_sw_stats` (`ionic_lif.h`), to
`ionic_lif_stats_desc[]` (`ionic_stats.c`) and to the `ionic.rst`
example, directly after `tx_unreadable_drop` — exactly where this change
adds `tx_unreadable_reseg`. So the two orders are not interchangeable:

- Tx only: apply `../0008` (or `../../devmem-tx-mainline/0007`) as the
  last patch. The Rx series then **does not apply** on top of it:
  Rx `0005` fails on `ionic_lif.h` and `ionic_stats.c`, and `0008` on
  `ionic_lif.c` behind it (checked with `git apply`).
- Tx and Rx: apply the Tx series **without** its frag-limit patch, then
  the Rx series, then the file here. That is the combination that was
  built and run.

## Which file applies where

| File | Base it applies to | Built? | Run? |
| --- | --- | --- | --- |
| `0007-ionic-send-an-unreadable-skb-with-too-many-frags-as-a-tso.patch` | `v7.4-rc3` + [`../../devmem-tx-mainline`](../../devmem-tx-mainline) `0001-0006` + the XDP frag offset fix + [`../../devmem-rx`](../../devmem-rx) `0001-0008` | yes, `W=1` and sparse, no warning | no |
| `0009-ionic-send-an-unreadable-skb-with-too-many-frags-as-a-tso.patch` | `v7.2.6` + [`../../srq`](../../srq) + [`../../p4`](../../p4) + [`..`](..) `0001-0007` + `../../devmem-rx-7.2.6` `0001-0008` | yes, `W=1`, no warning | **yes** — this is the file the runs used |

The numbers say where each goes: `0007` is the seventh patch of the
mainline Tx series, `0009` the ninth of a `v7.2.6` stack whose eighth is
`../0008`'s place. Neither is renumbered to `0001`, because neither is
the first patch of anything.

`0009` is a `diff -urNp` rather than a `git diff`; `git apply` and
`git am` take both.

## What was tested

- `0009`: generated on the staged `v7.2.6` + srq + p4 + devmem-tx + Rx
  tree, `checkpatch.pl --strict` clean but for the deliberate missing
  `Signed-off-by:`, and a `W=1` build of `ionic.ko` with no warning. The
  module built from it is the one the frag-limit runs used: every send
  that stalled without the fix completed with every byte verified at the
  receiver, over seven before-and-after layouts, a 300 s sustained run,
  an IPv6 pass, loss and MTU-change stress, and the 38-row devmem Tx
  matrix. See the commit message of `../0008` and
  [`docs/devmem/notes`](../../../../docs/devmem/notes)
  (`rocm102-frag-limit.md`).
- `0007`: `git apply --check` against the mainline Rx tree, `W=1` and
  sparse with no warning, `checkpatch.pl --strict` clean but for the
  `Signed-off-by:`. **Not loaded, not run**: no `v7.4-rc3` guest was
  brought up again for it.
- Neither file has been run through `git am`.

## Retiring this directory

Delete it when the frag-limit patch is folded into the Tx series and the
Rx series is regenerated on top of it — one tree, one order, no variants
— or when the whole devmem work is dropped.
