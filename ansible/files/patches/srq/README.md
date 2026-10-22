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

## The base: v7.2.4

The same ref `IONIC_KERNEL_REF` pins, and the kernel the CI guest and the
qemu-minimal fleet VMs actually run (`7.2.4-070204-generic`). That is the
point: a guest builds this with no kernel upgrade.

It did not start there. The upstream commits target `rdma/for-next` and
apply to **no** mainline tag on their own — three earlier for-next commits,
carried here as 0001-0003, get the series onto `v7.3-rc6`. Rebasing from
there down to `v7.2.4` took four context fixes, none of them semantic:

| # | What differed on 7.2.4 | Resolution |
| --- | --- | --- |
| 0001 | `ionic_ctx_resp` has no `phc_offset` | `rcq_sign_bit` + `comp_mask` still total the 4 bytes `rsvd2[3]` held, so the UAPI struct size is unchanged |
| 0004 | LIF identity has `rsvd1[162]` where 7.3 had `rsvd` + `rcq_sign_bit` + `rsvd1[160]` | `1+1+12+5+1+142 == 162`, so the offsets are identical — see below |
| 0006 | the `rq` → `rq.q` refactor, mechanical | resulting token census matches the 7.3 tree exactly |
| 0007 | returns `ib_respond_empty_udata(udata)`, not `0` | context only |

**The offsets were verified, not reasoned about.** 0004 moves
`srq_qtype` and `alloc_qid_cap` inside a reserved block, and those byte
positions are the wire contract `src/ionic_eth_emu.c` encodes as
`LIF_ID_SRQ_QTYPE_OFF` and `LIF_ID_ALLOC_QID_CAP_OFF`. Getting that wrong
fails silently — the driver reads the capability from the wrong offset and
the symptom is an absent SRQ, indistinguishable from the bug this series
fixes. So the layout is compiled and measured:

```
sizeof(logical_qtype) = 12
srq_qtype      offset = 96   (emulator expects 96)
alloc_qid_cap  offset = 113  (emulator expects 113)
sizeof(rdma ident)    = 256  (unchanged)
```

No 7.3-only API is involved. Every IB-core helper the series calls
(`ib_umem_get_va`, `ib_copy_validate_udata_in`, `ib_respond_udata`,
`rdma_udata_to_drv_context`) is already used by 7.2.4's own ionic. The one
7.3 rename, `ib_no_udata_io()`, is touched by none of the nine.

Verified end to end: `git am` applies all nine to a fresh `v7.2.4`, and the
result builds and installs as a DKMS package on `7.2.4-070204-generic`.

Building straight from an `rdma/for-next` SHA was rejected because that
branch is rebased and force-pushed: a pin there tests something different
every run, and a regression cannot be bisected to a change of ours.

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
