# SRQ patch series for the ionic drivers

The upstream SRQ series, reworked to apply to a **mainline tag** rather than
to a moving branch. Applied by `fetch-ionic-sources.sh --patches-dir` and
built by the `ernic_srq_dkms` role.

## Why this directory exists

Without SRQ the guest reports `max_srq = 0`. UCX's `rc_verbs` transport is
built on a shared receive queue — its whole receive path is
`ibv_post_srq_recv()` — so it declines the device, leaving `ud_verbs` as the
only verbs transport. UD has no one-sided RDMA READ/WRITE, so NIXL (and with
it LMCache P2P) silently falls back to TCP. A zero there is the difference
between RDMA and not.

## The base: v7.3-rc6

Measured, not assumed. The five upstream SRQ commits alone apply to **no**
mainline tag:

| Base | Result |
| --- | --- |
| `v7.2` | all five fail |
| `v7.2.9` (latest stable) | all five fail — its ionic is byte-identical to `v7.2` |
| `v7.3-rc6` | the net-side patch applies; all four RDMA patches fail |

The RDMA half depends on three earlier `rdma/for-next` commits that are not
in mainline. Including those as 0001-0003 makes the whole series apply
cleanly to `v7.3-rc6`, which is what this directory does.

### Why not back-port the nine to `v7.2.4` as well

The obvious question, since `v7.2.4` is what `IONIC_KERNEL_REF` pins and what
the CI guest image runs — a series applying to both would need no image
rebuild. Measured, with all nine:

| How | Result on `v7.2.4` |
| --- | --- |
| `git am` | fails at **0001**, on `include/uapi/rdma/ionic-abi.h:46` |
| `git am -3` | cannot run — the sparse checkout has no blobs to merge from |
| `patch -p1 -F3` (fuzz) | **7 of 9**; 0004 and 0006 reject one hunk each |

The first blocker looks trivial: the only `ionic-abi.h` difference between
the two bases is one added line (`__aligned_u64 phc_offset`), so 0001 fails
on context, not on anything semantic. That is what makes this worth writing
down rather than just asserting — it *looks* like a small job.

It is not, and the reason is specifically the hunk 0004 rejects:

```diff
 			u8 rcq_sign_bit;
-			u8 rsvd1[160];
+			struct ionic_lif_logical_qtype srq_qtype;
+			u8 rsvd2[5];
+			u8 alloc_qid_cap;
+			u8 rsvd1[142];
```

That is the **LIF identity wire layout** — `srq_qtype` and `alloc_qid_cap`
at fixed byte offsets inside a reserved block, which is exactly the contract
`src/ionic_eth_emu.c` encodes as `LIF_ID_SRQ_QTYPE_OFF` and
`LIF_ID_ALLOC_QID_CAP_OFF`. Resolving that reject by hand means
re-deriving a wire format from reserved-field arithmetic. Get it wrong and
nothing fails to build: the driver reads the capability from the wrong
offset and the symptom is a malformed or absent SRQ capability, which is
indistinguishable from the bug this whole series exists to fix.

Fuzz makes that worse rather than better. "Hunk #3 succeeded with fuzz 3"
means `patch` guessed the location, and for offset-bearing structures a
good guess and a correct one are not the same thing — which is why the
clean-`git am` requirement is not fussiness here.

So the base moves forward, not back: repin the guest image to a kernel
matching this ref. The series is queued for 7.4 and this directory retires
when it lands (below).

The alternative — building straight from an `rdma/for-next` SHA, which
already contains everything — was rejected because that branch is rebased and
force-pushed: a pin there tests something different every run, and a
regression cannot be bisected to a change of ours.

## Order matters

`fetch-ionic-sources.sh` applies `*.patch` in glob order, so the numbering is
the dependency order. Do not renumber without re-testing.

| # | Commit | Why it is here |
| --- | --- | --- |
| 0001 | `9951bf4c44c6` RCQ userspace support | prerequisite |
| 0002 | `2eca9c14e66a` Fix XArray initialization | prerequisite |
| 0003 | `485effd117d0` Fix double removal of CMB mmap entries | prerequisite |
| 0004 | `c73a5f20bc78` net: fetch qid allocation and SRQ capability | adds `srq_qtype` + `alloc_qid_cap` to the LIF identity |
| 0005 | `1900af7931ab` firmware-assigned CQ IDs | |
| 0006 | `0d0c461f814b` segregate rq fields into `ionic_rq` | refactor the SRQ support needs |
| 0007 | `8a093abb541a` **add Shared receive queue (SRQ) support** | the verbs |
| 0008 | `4dd2160af277` SRQ event handling | |
| 0009 | *(local)* allocate an address handle for UC queue pairs | this repo's own patch, carried so a tree built from here is not a regression against `patches/` |

0009 is a copy of `patches/0002-*.patch`. `--patches-dir` **replaces** the
default directory rather than adding to it, so a local patch omitted here
would silently be dropped from an SRQ build.

## What the result must contain

After applying, these are true — the role's guard checks the first and a
build that loses them is the failure mode worth catching:

```
drivers/infiniband/hw/ionic/ionic_controlpath.c   ionic_create_srq_cmd present
drivers/infiniband/hw/ionic/ionic_ibdev.c         attr->max_srq = dev->lif_cfg.srq_count
include/uapi/rdma/ionic-abi.h                     __u32 srqid
```

`include/uapi/rdma` had to be added to the sparse checkout in
`fetch-ionic-sources.sh` for this: the series touches `ionic-abi.h`, so
without that path `git am` fails on a file that is not checked out, and a
DKMS build would compile against the running kernel's older ABI header and
quietly lose `srqid`.

## Retiring this directory

The series is queued for 7.4. When it lands in a release, drop 0001-0008,
point `ernic_srq_dkms_sources` at that tag, and keep only the local patch —
carrying upstream commits as patches past the point where they are upstream
is how a tree ends up applying a patch twice.
