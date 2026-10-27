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
reaches a very confusing state: the kernel advertises `max_srq = 256`, and
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
rewritten during review. Vendoring the upstream commits makes the input reviewable
in a diff and stable across rebuilds.

## Order matters

Applied in glob order on top of the base:

| # | Commit | Why it is here |
| --- | --- | --- |
| 0001 | Update kernel headers | syncs `kernel-headers/rdma/ionic-abi.h`, incl. `srqid` |
| 0002 | ionic: Restructure receive queue initialization for SRQ | refactor the verbs need |
| 0003 | ionic: simplify `rq_meta` allocation | |
| 0004 | **ionic: Add SRQ support** | `create`/`destroy`/`modify`/`query_srq`, `post_recv_srq` |
| 0005 | ionic: ring the RQ qtype for SRQ receives | **local fix**, see below |
| 0006 | ionic: stop on a failed SRQ receive prepare | **local fix**, see below |

### 0005 and 0006 are ours, not upstream's

0001–0004 are [#1799][pr] as exported. The last two are corrections written
here, and they are **required** — do not drop them when the upstream series
is rebased or retired. Re-check both against whatever replaces #1799 and
carry them forward if the defects are still present.

**0005 — the doorbell qtype.** Both SRQ receive paths rang `ctx->cq_qtype`
where the device expects `ctx->rq_qtype`, which is what the ordinary receive
path a few lines away already rings. A receive doorbell sent to the CQ qtype
arrives as a completion-queue consumer-index update for whatever CQ shares
that id, so the SRQ never learns a receive was posted. The effect is a hang,
not an error: a bound QP completes its sends and never completes a receive.
`ionic_post_recv_srq()` is the obvious one; `ionic_post_pending_srq_wrs()`
is the same mistake on the deferred path, and only runs once the hardware
queue has filled, so light-load testing never reaches it.

**0006 — error handling in the post loop.** `ionic_post_recv_srq()` ignored
what its two preparation helpers returned, then counted the work request as
posted and walked past it. A failure left `in_use` drifting upward until the
queue reported itself permanently full, pointed `*bad` at the wrong work
request, and reported success to the caller.

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
