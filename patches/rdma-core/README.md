# rdma-core SRQ patch series for the ionic provider

The **userspace half** of the SRQ work. `../srq/` patches the kernel driver;
this patches rdma-core's `providers/ionic`. Both are needed — either alone
produces a guest that cannot create an SRQ.

## Why this directory exists

`ernic_guest_setup`'s `rdma_core.yml` says it uses "stock rdma-core, whose
`providers/ionic` has been upstream since v61". True, and insufficient:
**no rdma-core release has SRQ support in `providers/ionic`, and neither does
`master`.** Checked directly against the upstream tree:

| Ref | `create_srq` in `providers/ionic/ionic_verbs.c` |
| --- | --- |
| `v61.0` | none |
| `v62.0` | none |
| `v65.0` | none |
| `master` | none |

It is still open as [linux-rdma/rdma-core#1799][pr]. Without it the guest
reaches a very confusing state: the kernel advertises `max_srq = 32768`, and
`ibv_create_srq()` returns `EOPNOTSUPP` — because libibverbs dispatches to a
provider that has no `create_srq` op at all.

Downstream that is the whole ballgame. UCX's `rc_verbs` transport is built on
a shared receive queue, so it declines the device and leaves `ud_verbs`, which
has no one-sided RDMA READ/WRITE — so NIXL, and with it LMCache P2P, silently
falls back to TCP.

[pr]: https://github.com/linux-rdma/rdma-core/pull/1799

## The base: `bd3282a1cf1025c7869a8664c2d54266a4dded44`

A SHA on `master`, not a tag, because the series does not apply to any tag.

**Why these are files and not a `curl` of the PR.** A GitHub PR patch URL is
not immutable: the author force-pushes and the same URL yields different code,
with nothing to notice it. That is the identical objection `../srq/README.md`
raises against pinning `rdma/for-next` — "a pin there tests something
different every run, and a regression cannot be bisected to a change of ours"
— and it applies with more force here, because a PR is *expected* to be
rewritten during review. Vendoring the four commits makes the input reviewable
in a diff and stable across rebuilds.

## Order matters

Applied in glob order on top of the base:

| # | Commit | Why it is here |
| --- | --- | --- |
| 0001 | Update kernel headers | syncs `kernel-headers/rdma/ionic-abi.h`, incl. `srqid` |
| 0002 | ionic: Restructure receive queue initialization for SRQ | refactor the verbs need |
| 0003 | ionic: simplify `rq_meta` allocation | |
| 0004 | **ionic: Add SRQ support** | `create`/`destroy`/`modify`/`query_srq`, `post_recv_srq` |

## Deliberately NOT paired with PR #1733 (RCQ)

The obvious companion, since `../srq/0001` carries the *kernel* RCQ commit as
a prerequisite. It is excluded for two reasons, and the second is the one that
matters:

1. It collides with #1799 in `ionic_verbs.c` — applying #1733 then #1799
   leaves one rejected hunk in `ionic_qp_sq_destroy`'s neighbourhood. The two
   series are both written against `master`, not stacked.
2. **It is not needed.** The worry is that our kernel's `ionic-abi.h` and this
   provider's disagree, since our kernel carries the RCQ fields and this base
   does not. Compare the two layouts:

   ```
   ours (7.2.4 + ../srq)     ... expdb_qtypes, rcq_sign_bit(1), comp_mask(2)
   this base                 ... expdb_qtypes, rsvd2[3],        phc_offset(8)
   ```

   `rcq_sign_bit` + `comp_mask` occupy **exactly** the three bytes upstream
   still calls `rsvd2[3]`, so the provider reads them as reserved and ignores
   them — which is correct, because it implements no RCQ. `phc_offset` is
   declared but **never read** by the provider (zero references in
   `ionic_verbs.c` and `ionic.h`), and libibverbs zero-initialises the
   response buffer, so the kernel writing a shorter struct is harmless.

   Symmetrically, `struct ionic_qp_req` is an *input*: this provider sends the
   shorter form, and `ib_copy_validate_udata_in()` zero-fills the tail, so our
   kernel's `req.ionic_flags` / `req.rsvd_pad` checks see 0 and pass.

Verified rather than argued: built against this base plus #1799 and run on a
`7.2.4-070204-generic` guest, `ibv_create_srq` succeeds and UCX offers
`rc_verbs` on the device.

## Retiring this directory

Delete it when #1799 merges and `ernic_rdma_core_version` can name a release
that contains it. Carrying upstream commits as patches past the point where
they are upstream is how a tree ends up applying a patch twice.
