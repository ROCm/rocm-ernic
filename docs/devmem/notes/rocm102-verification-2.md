# Stack 3 (`ionic-devmem-r102`): independent verification of the overnight results

2026-11-20, 17:40Z to 18:20Z. `V` = `/var/tmp/ionic-devmem/overnight2/verify`,
`VE` = `V/evidence`, `B` = `/var/tmp/ionic-devmem/test-rocm102`. I am a second
verifier, working only on stack 3. Stacks 1 and 2, `rocm-ernic`, `qemu-minimal`
and `kernel-tools` were not touched; nothing was committed, pushed or posted;
no product code was changed. Everything below was **RUN by me** unless marked
**NOT RE-RUN** (the engineer's evidence exists and I audited it) or **NOT RUN**
(nobody ran it).

The reports verified: [rocm102-p2p-kernel.md](rocm102-p2p-kernel.md),
[impl-rx-backport-7.2.6.md](impl-rx-backport-7.2.6.md),
[rocm102-rx.md](rocm102-rx.md), [rocm102-frag-limit.md](rocm102-frag-limit.md),
[rocm102-gpu-to-gpu.md](rocm102-gpu-to-gpu.md).

## 0. Summary

**The five headline results hold.** The GPU-to-GPU claim in particular survives
a re-run with my own random seed, my own expected-byte generator, my own
placement probe and my own host-side hash, with two checks the original run did
not have (a real negative control and a poison check). The kernel really is
stock v7.2.6 plus exactly one commit. The RX backport really changes no code.
The frag-limit fix really turns `ETIMEDOUT` into verified transfers, and it does
so in a matched pair that removes the one methodological objection to the
engineer's table.

What does not hold is some of the bookkeeping around the GPU-to-GPU result: one
table row built from the wrong run, one number that exists nowhere, two findings
whose diagnostic captures were never saved, and a "negative control" that is a
block of zeros. None of these touches the headline; all of them should be fixed
before the note is used as evidence.

| Claim | Verdict |
| --- | --- |
| `7.2.6-devmemp2p` = stock v7.2.6 + exactly 69ab87acc614, full config | **CONFIRMED** |
| RX backport: 8 patches, no code change, builds clean at every patch | **CONFIRMED** |
| RX backport locking table against real v7.2.6 source | **CONFIRMED** (8 of 8 sites I picked) |
| Frag-limit fix: stalls become completed, verified transfers | **CONFIRMED** (my own matched pair) |
| Frag fix never writes shared `skb_shinfo`, never reads payload | **CONFIRMED** |
| Devmem RX works (udmabuf, random data, TX -> RX) | **CONFIRMED** (re-run) |
| RX-capable driver behaves as the plain one when split is not offered | **CONFIRMED** (re-run) |
| **VRAM -> VRAM, all four links in one run** | **CONFIRMED** (re-run, strengthened) |
| "every RX payload descriptor in BAR0" | **REFUTED as written** (64 are not) |
| Row a's TX frag count (17611) | **REFUTED** (that is run A1's number; A2's is 17560) |
| Row c's "23455 in BAR0" | **REFUTED** (no such number; C2's is 23262) |
| "no `dma_sync` on a P2P address" | **WEAK** (the probe never fires at all) |
| The host-side hash as an independent arrival check | **WEAK** (same backing store) |
| F1 (GPU-kernel hang is outside devmem) | **WEAK** (conclusion plausible, diagnosis unevidenced) |
| F2 (1 MiB RX dma-buf starves the pool) | **CONFIRMED clean**; its quoted `ss` numbers are absent |

**New, from cases nobody had run** (section 6): with one combined channel and a
live devmem RX binding, **ordinary TCP on the same interface receives nothing**,
which breaks a concurrent RDMA dma-buf test's out-of-band exchange. Killing the
receiver mid-transfer with a VRAM binding is clean. Both directions at once
works.

## 1. Identity: kernel, modules, servers

### 1.1 The kernel — CONFIRMED

A full `diff -r` of the source tree the kernel was actually built from
(`K/kbuild-full/src`, out-of-tree build, no artifacts) against pristine v7.2.6:

    Files .../linux-7.2.6/net/core/devmem.c and .../kbuild-full/src/net/core/devmem.c differ

**That is the complete list: exactly one file.** A second pass over 101,074
entries found name, type and mode identical on both sides. The `diff -u` of that
one file is byte-for-byte the packaged patch, and the patch's added and removed
lines are line-for-line upstream `69ab87acc614` (compared against the object in
the local v7.4-rc3 tree, which has it): 37 insertions, 10 deletions, same order.
`K/scripts/kbuild-full.sh` does `git archive v7.2.6` then a single `git apply`;
the build log has no other `git apply`, `patch -p`, `Hunk` or `quilt`.

Binary-level: `strings` on the shipped `vmlinux` finds `Failed to pin dmabuf`
and `Failed to wait for dmabuf fences`, neither of which exists anywhere in
pristine v7.2.6's `net/`.

Config: the config inside the image `.deb` is identical to the build's `.config`,
which differs from the guest's stock `config-7.2.6-070206-generic` (sha256
`3599ef6c...`, as claimed) in exactly 4 lines — `CC_VERSION_TEXT`,
`PAHOLE_VERSION`, and the two emptied key paths. `sha256sum -c` on the three
debs: all OK. The vmlinuz banner is
`7.2.6-devmemp2p (devmem@testbed) #3 SMP PREEMPT_DYNAMIC Fri Nov 20 03:33:55 UTC 2026`.

**Running now**, checked by me in both guests: `uname -a` is exactly that string
on r102-1 and r102-2 (`VE/00-as-found-driver-state.txt`, `VE/99-final-state.txt`).

Two residual weaknesses, neither contradicted: the "guest's `/boot` config" I
compared is the host's copy (I did not re-read `/boot` in the guests), and I did
not prove object-by-object that the 33 GB build tree was fully recompiled rather
than partly inherited from the earlier `localmodconfig` attempt.
Evidence: `VE/kernel-*.txt`.

### 1.2 The modules — CONFIRMED

- Loaded = built, on every load I did (3 loads x 2 guests x 3 modules, **18 of
  18 `=> SAME`** on srcversion *and* build-id): ionic `0FDEA047AB0132E8F7AB9C8`
  / build-id `626c0279...`, `ionic_rdma` `24E57766...` / `631bb321...`,
  `ernic_p4` `66D2E219...` / `3e60a75f...`.
- The staged sources are the same everywhere: the 28 ionic files in
  `B/ionic-mod/rxfrag/tree/.../ionic/` are **byte-identical** to
  `~/ionic-mod/rxfrag/tree/...` in **both** guests (sha256 per file; the only
  difference is `ls` sort order).
- `O/tree` (`overnight2/rx-backport`) is `diff -r`-identical to
  `B/ionic-mod/rxbp/tree`, and `rxfrag/tree` differs from `rxbp/tree` in exactly
  three files, whose `+`/`-` lines are byte-identical to the frag patch.
- Note for the record: `rocm102-frag-limit.md` §2 calls `631bb321...` and
  `3e60a75f...` "srcversion"; they are build-ids. The srcversions are
  `24E57766...` and `66D2E219...`.

### 1.3 The servers — CONFIRMED

`B/ernic/poc-rel/SOURCE` names `2d3a3d02f58001d366114293deaa58bcf1d63d1f`,
build type Release; the binary's sha256 is `d50b1d29...`, as the kernel note
records. Both containers bind-mount it at `/opt/verify/rocm-ernic.real` behind
`hs-wrapper.sh`, and both log `NIC profile: compat + header-split (hash
0x765791b9b9cd1313)` and `NIC Rx header split: on`.

## 2. Source checks

### 2.1 The RX backport — CONFIRMED

For each of the 8 patches I extracted every `+`/`-` line (ignoring `@@` headers
and file headers) and compared, as multisets and in order, against **two**
independent upstream copies: `patches-rx/mainline/` and the committed series in
the `rocm-ernic` working tree (read only). All 8 are **byte-identical** in code
lines against both. The only differences anywhere: the `Documentation/` hunks
dropped from 0003 and 0005 (disclosed), one context line in `ionic_phc.c`
(disclosed, and matching real v7.2.6 source), the subject tag and `Date:`.
`sha256sum -c` of `O/evidence/patches.sha256`: 8 of 8 OK.

Build-clean at every patch: the per-patch logs cover all ten steps (`base`,
`b1`..`b8`, `tree`) in three legs. The W=1 leg has **zero** lines matching
`warning|error` anywhere; the sparse leg has 0 diagnostics and its negative
control does emit one, so the check is live. The stages are genuinely
incremental: for each N, `diff -r b(N-1) bN` is exactly patch N's lines.

**Locking table against real v7.2.6 source**: I picked eight call sites and
checked each in a pristine v7.2.6 tree (`k726`, `git describe` = `v7.2.6`). All
eight claims are right, e.g. `net/core/netdev_work.c:130` really does
`rtnl_lock()` then `netdev_lock_ops(dev)` (so `ndo_set_rx_mode_async` runs under
both on v7.2.6, instance lock only on v7.4); `net/core/dev.c:11389-11392` really
only `netdev_WARN`s where v7.4 adds `return -EINVAL`; `netdev_nl_bind_rx_doit()`
at `netdev-genl.c:1013` really takes the instance lock and no RTNL;
`devmem.c:229` really is the plain `dma_buf_attach()`. One nit: the note cites
`dev.c:11389` (the `if`); the WARN is at 11392.
Evidence: `VE/rxbp-01..16*.txt`.

**Caveat the note states and I repeat**: this is compile-and-apply evidence
only, and the `.ko` link was only ever done against the locally built
`7.2.6-devmemp2p`, never against the stock guest kernel's `Module.symvers`.

### 2.2 The frag patch — CONFIRMED, with one clarification

- **No write to shared `skb_shinfo` state.** `ionic_tx_unreadable_reseg()`
  (`ionic_txrx.c:1900`) takes `const struct skb_shared_info *shinfo` and its only
  output is `*mss`, a pointer to a **caller stack local** (`unsigned int mss;` at
  2133 / 2097). Every other shinfo access on the path is a read; frags are never
  modified. The file's only two shinfo writes are pre-existing hwstamp ones, and
  the reseg path refuses the hwstamp queue first (1913).
- **No payload read.** Nothing on the path calls `skb_frag_address`,
  `page_address`, `kmap`, `memcpy`, `skb_copy`, `skb_checksum`, `csum_partial`
  or `skb_linearize` (2069-2073 returns before the linearize). It reads only the
  linear headers and frag **sizes**, after proving the headers are entirely
  linear (1921-1926). The checksum is the device's: the driver writes only the
  pseudo-header seed, computed from the IP addresses.
- **Clarification, not a fault.** The fix does newly write the skb's **linear
  header area** on non-GSO unreadable skbs, via `ionic_tx_tso()` ->
  `ionic_tx_tcp_pseudo_csum()` -> `skb_cow_head(skb, 0)` (a no-op for a TCP
  transmit clone) + the pseudo-csum store at 1577. The patch's own commit message
  says this explicitly ("That function prepares the headers in the linear area
  after `skb_cow_head()`, as it does for every GSO skb, and those are the
  clone's own"), so the claim as written is accurate; a reader should just not
  read "the skb is not changed" as covering the header bytes. Benign: TCP
  rebuilds both headers on every (re)transmit.
- **The descriptor limit holds on every path.** mss is `min` over the sliding
  window of `max_sg_elems` consecutive frags, so no descriptor can span
  `max_sg_elems + 2` frags; `desc_nsge` (`u8`) cannot exceed 15; `ndescs` is
  bounded by `ndescs > q->num_descs / 2 -> -E2BIG` and is the same count the dry
  run and the post use. Zero-size frags are rejected (1958), so mss >= 1 and the
  `DIV_ROUND_UP` cannot divide by zero. The only latent divide-by-zero needs
  `q->max_sg_elems == 0`, which `ionic_lif.c:4295` makes unreachable (it
  overwrites the firmware value with 15). I found **no** path on which an
  over-limit skb still reaches the device over the limit.
- Two cheap hardenings worth having if this ever lands upstream: `*mss = 0` at
  the top of `ionic_tx_descs_needed()` (one early return leaves it unset, safe
  today only because both callers check `ndescs < 0` first), and a `max_sg_elems`
  guard.
- The `v7.2.6-on-rx/0009` form's `+`/`-` lines are byte-identical (129 lines) to
  the `v7.2.6/0008` form's, as claimed.
Evidence: `VE/frag-*.txt`.

## 3. Re-runs

All with my own random seeds, my own `V/scripts/vpat.py` (which derives the
sender buffer offset itself rather than reusing the predecessor's iovec walker;
the two agree on four spot checks, including reproducing the B1 hash
`cda2d358...` from the note), my own `V/scripts/vprobe.bt` and my own
`V/scripts/vcase.sh`.

### 3.1 `V1-vram2vram` — VRAM -> VRAM, 64 MiB, seed 1331473080 — **CONFIRMED**

All four links, in one run, in one file (`VE/V1-vram2vram.txt`):

| Link | What I measured |
| --- | --- |
| Sender descriptors in BAR0 | `@tx_frag[GPU BAR0]: 17533` — **and no "not BAR0" bucket at all**, min `0x4038000000` max `0x403c000000` |
| Receiver posted buffers in BAR0 | `@rx_niov[GPU BAR0]: 38059` (no other bucket); `@rx_desc_payload[GPU BAR0]: 46482` |
| Both buffers VRAM while bound | `VRAM VISIBLE pin count 1 exported` on **both** guests; `@pin_domain_mask[6]`, `@attach_kind[dynamic allow_peer2peer=1]`, `@attach_peer2peer[true]`, `@p2pdma_distance[4]` on both |
| Bytes in the receiving GPU's memory | three fragments, host-hashed from the **receiver's** server mapping, all EQUAL |

    frag stream_pos=67107192 dmabuf_off=32321536 len=1448 -> BAR0 0x39ed3000
       expected(host,vpat.py) 2ef1b294...  memfd of receiver srv 2ef1b294...  => EQUAL
    (and two more, with a real negative control between them: differs-as-it-must)

Receiver: `RESULT rx PASS ... mismatches=0 mismatch_a5=0 mismatch_zero=0`,
`linear_cmsgs=0`, `tokens_returned == dmabuf_cmsgs`, `out_of_buffer=0`.
0 dma-buf objects, 0 page pools, 0 pinned BOs and 0 splat lines afterwards; all
five emulator error counters 0 on both servers.

**Two checks the original run did not have:**

- **A real negative control.** I hashed one fragment's expected bytes against
  another fragment's address: differs, as it must. The predecessor's control is
  `de2f2560...`, read 1 MiB **past the end** of the dma-buf at a different
  length — I confirmed by computation that this constant is simply
  `sha256(64 KiB of 0x00)`. It is vacuous.
- **A poison check.** The last 64 KiB of the bound buffer hashes
  `77007cd7...` = `sha256(64 KiB of 0xa5)`, i.e. **still the pre-fill**. So the
  NIC wrote the plan's bytes where the plan said and nothing else, and the
  result cannot be a wholesale fill.

**What the host hash does and does not prove.** The server's
`/memfd:rocjitsu-vram` and the receiver's GEM mapping are the *same backing
store*, so this is not a second, independent observation that the bytes arrived.
What it genuinely proves is placement: the DMA address the driver handed the
device maps to exactly where the correct bytes are. Section 2's framing in
rocm102-gpu-to-gpu.md ("independently on the host") overstates this slightly.
**WEAK as framed, strong as a placement proof.**

### 3.2 `V2-udma-rx` — devmem RX with udmabuf, 64 MiB — **CONFIRMED**

`RESULT rx PASS ... mismatches=0 mismatch_a5=0 mismatch_zero=0`,
`linear_cmsgs=0`, 46638 cmsgs all returned. Every address in the **not BAR0**
bucket on both sides, which is the control that my BAR0 classifier is not
trivially positive.

### 3.3 `X5-offloads-off` — tx off / sg off / both — **CONFIRMED**

All three: sender `ETIMEDOUT` / `EAGAIN` after one short send, receiver
**0 bytes**, and

    mismatches=0 mismatch_a5=0 mismatch_zero=0   (all three cases)

So **no zero-filled payload is accepted**, which is the point of the row. (The
receiver pre-fills 0xa5 and counts zero and poison mismatches separately, so a
zero-filled payload could not be mistaken for a short transfer.)
`tx_unreadable_drop` +0, +0, +10 — the first two stall above the driver.

### 3.4 `X6-frag-rxbp` / `X6-frag-rxfrag` — the frag pair — **CONFIRMED**

Run as a **fair pair**, which the engineer's table was not: the same
`net.ipv4.tcp_retries2 = 5` and the same `vtx` options on both sides (the
original "before" runs used `retries2=5` and `-C 2` while the "after" runs used
15 and the default — that does not change the bytes or the frag layout, only how
fast a stall surfaces, but it made the pair unmatched). Payload is fresh
`/dev/urandom` per run; the receiver hashes what it got and the sender hashes
that many bytes of its own expected file.

| Spec | Before (`rxbp`, ionic `7C12352B...`) | After (`rxfrag`, ionic `0FDEA047...`) |
| --- | --- | --- |
| `d:9:960:64:4032:4096` (15 x 64 B) | `ETIMEDOUT` after 13.1 s, 2880 B accepted, **receiver 0 B**, drop +7 | **288000 B, sha256 equal**, 300 of 300 completions, drop +0 |
| `d:5:32768:64:16:4099` (512 x 64 B) | `ETIMEDOUT` after 14.6 s, 2176 B accepted, **receiver 0 B**, drop +8 | **9830400 B, sha256 equal**, drop +0 |
| `d:5:65536:128:16:4099` (control) | completes, 19660800 B, sha256 equal | completes, 19660800 B, sha256 equal |

The control behaves identically on both sides, so the pair is a real
before/after and not a configuration difference.

### 3.5 `X8-compat-profile` — the RX driver where split is not offered — **CONFIRMED**

Stack cold-cycled onto the plain `poc-rel` server (`NIC profile: compat`), the
`rxfrag` set loaded (6 of 6 `=> SAME`). On **both** guests: `TCP data split:
n/a`; `tcp-data-split on`/`off` refused with
`ionic: Device does not support tcp-data-split` / `Operation not supported`;
`auto` accepted; `rx-checksumming: off [fixed]`; a devmem RX bind refused with
`netlink error -22 ... extack='tcp-data-split is disabled'`. Upstream
`quick-start test` base: **rc 0**. 64 MiB hashed TCP both ways: PASS/PASS.
That is exactly the "feature off" row of rocm102-rx.md 4.4.

### 3.6 Final configuration — **CONFIRMED**

Back on the header-split server with `rxfrag`: `quick-start test` base **rc 0**;
`--rocm-rdma-test --rocm-rdma-mode dmabuf` **rc 0**, 1560.31 MiB/s,
`VALIDATION: PASSED - 7 chunks, 58720256 bytes`; a 16 MiB VRAM -> VRAM devmem
smoke PASS with 0 mismatches.

## 4. What the evidence does not support

These are bookkeeping faults in `rocm102-gpu-to-gpu.md`. I checked each against
the raw files myself.

1. **"every RX payload descriptor in BAR0"** (§4 row e, §5) — **REFUTED as
   written.** `B1`, `C2`, `E1`, `C4`, the 1020 MiB and 960 MiB runs and the 1 MiB
   runs each have `@rx_desc_payload_addr[System RAM]: 64`. **My own V1 run has
   exactly 64 too**, so it is a systematic, benign pre-bind artefact — but the
   report annotates it only on row a and drops it silently from the others.
2. **Row a's "17611 System RAM"** — **REFUTED.** `A2-udma-to-vram.txt:131` says
   **17560**. 17611 occurs only in `A1-udma-to-vram.txt`, a different run. The
   row is labelled A2.
3. **Row c's "23455 in BAR0"** — **REFUTED.** That number appears in no file.
   `C2-hip-to-hip.txt` has `23262` BAR0 + 64 System RAM.
4. **F1's hang diagnosis** — **WEAK.** `grep` over all 79 evidence files finds no
   `amdgpu_fence_info`, no `State: R`, no CPU-percentage capture and no
   "fresh HIP process ran normally" check. The conclusion that **devmem is not
   implicated** is still well supported by what *is* there (every byte delivered
   and verified with 0 mismatches up to the stall; C2 and C4 passed the same
   shapes). But the file also contradicts the report's "verified 25577320 bytes
   ... over 17 batches and then stopped": lines 120-121 show it reaching
   `bytes=28591976 ... batches=19` at t=290.
5. **F2's TCP numbers** — the quoted `ss -ti` line
   (`cwnd:1 backoff:7 rto:39424 lost:207 rwnd_limited:99.7%`) is **absent**:
   `D-1m-stall-diagnosis.txt` has `ss` header rows with no data. "it is the same
   with udmabuf" has no 1 MiB udmabuf run behind it. **F2's substance is
   CONFIRMED**, though: `RESULT rx FAIL ... mismatches=0 mismatch_a5=0
   mismatch_zero=0 end=idle-timeout`, `tokens_returned=18712 == dmabuf_cmsgs`
   (no double-free, no leak), `out_of_buffer=0`, `rx_dma_error 0`, with
   `@rx_niov_alloc_failed: 9995` against 256 net_iovs. **The failure is clean:
   the stream stalls, nothing is corrupted.** I saw the same mechanism myself in
   my bidirectional run (450 allocation failures on the guest doing both
   directions with a 32 MiB buffer) and that transfer still completed and
   verified, so the mechanism is real and graceful.
6. **"No `dma_sync` on a P2P address"** (§5) — **WEAK, as instrumented
   unfalsifiable.** The predecessor's probe counts only BAR0 hits. Mine counts
   **both** buckets, as a positive control: across six runs on both guests,
   `@dma_sync` is absent entirely — the `__dma_sync_single_for_cpu/device`
   kprobes never fired at all. So the statement is true but carries no
   information; a probe that cannot fire cannot find a sync.
7. Several counters are reported as per-run deltas when they are ethtool
   **lifetime** counters (E1's "`rx_unreadable_drop` +24", the constant
   `tx_unreadable_drop: 121`).

## 5. Not re-run

I audited the evidence for these and found it internally consistent, but I did
not re-run them: the 35-case udmabuf devmem TX matrix; the 4130 bind/unbind
leak loop and `slab-check`; the 330 s and 200 s sustained runs; sizes above
64 MiB (256 MiB, 960 MiB, 1020 MiB); the stock-kernel control (case f); the
KFD eviction loop (24 iterations); the whole ops-locking suite and the lockdep
boot; the header-split-with-ordinary-pages matrix (4.4) except its "feature off"
row; `--rocm-rdma-mode vram`'s `error=14` source argument; the frag work's
wire captures, netem and jumbo rows.

**NOT RUN by anyone**, from my own list: the receiving GPU buffer freed by a
userspace process that stays alive (`devmem_rx_gpu` has no option for it and I
did not write a new test binary; the SIGKILL case below covers process death,
and `bufinfo` shows the binding holds a reference, so a userspace close alone
cannot free the backing store — that last part is inference, not a run).
See `VE/X7-free-while-bound.txt`.

## 6. Cases nobody had run

### N1. Ordinary TCP is dead on the receiver while a devmem binding holds the only queue — **new, and it bites**

`VE/X4-rdma-during-devmem.txt`, `VE/X4b-rdma-control.txt`,
`VE/X4c-plain-tcp-under-binding.txt`.

Running `ib_write_bw --use_rocm=0 --use_rocm_dmabuf -F --data_validation`
(the quick-start dmabuf RDMA test's own command) **during** a 300 MiB VRAM ->
VRAM devmem transfer: the devmem transfer passed (`RESULT rx PASS
bytes=314572800 ... mismatches=0`, `linear_cmsgs=0`), but the RDMA run died in
its out-of-band exchange (`ethernet_read_data: Connection reset by peer`).

The control settles it: **the same RDMA command alone, in the same
configuration, passes** — 1181.85 MiB/s, `VALIDATION: PASSED - 7 chunks,
58720256 bytes`.

The cause is not the devmem *transfer* but the devmem *binding*. With
`combined 1` and `tcp-data-split on`, every flow lands on the bound queue, whose
payload buffers are net_iovs. I tested that directly: with a binding live and
**no sender at all**, 4 MiB sent to an ordinary `nc` listener on the receiver
delivered **0 bytes** (`-rw-rw-r-- 1 ubuntu ubuntu 0 /tmp/x4c-nc.bin`), and
`rx_unreadable_drop` did not move (11 before, 11 after) — the payload went into
the dma-buf and the socket, which did not ask for `MSG_SOCK_DEVMEM`, simply
could not see it.

This follows from the design, but it is written down nowhere, and it bounds
"devmem and RDMA coexist on the same NIC": any ordinary TCP on that interface —
perftest's control connection, ssh, a health check — stops working for as long
as a binding holds the only queue. Worth a line in the notes, and a reason to
prefer more than one channel with queue steering.

### N2. Receiver killed mid-transfer with a live VRAM RX binding — clean

`VE/X2-killrx.txt`. SIGKILL 6 s into a 64 MiB VRAM GEM binding. Afterwards, both
immediately and 5 s later: dma-buf objects **0**, pinned/exported BOs **0**,
`mem_info_vram_used` back to the byte (`69963776 -> 2838528`), 0 dmabuf page
pools, 0 orphan/detached pools, **0 splat lines**. The sender saw a clean reset
(`so_error=104`). A fresh VRAM BO was then created, bound and used:
`RESULT rx PASS ... mismatches=0`, `@rx_niov[GPU BAR0]: 3932`,
`@tx_frag[GPU BAR0]: 2224`.

Harness note: on that error path `devmem_tx_ext2` printed
`completed=4294967361`, an unsigned wrap in its own counter. Cosmetic, but it
would mislead anyone reading a failed run's output.

### N3. Both directions at once — works

`VE/X3-bidir.txt`. Each guest simultaneously sending from its own VRAM BO and
receiving into another, 32 MiB each way. Both directions `RESULT rx PASS ...
mismatches=0`; on each guest the TX frags and RX net_iovs are entirely in GPU
BAR0; emulator error counters 0 on both servers; 0 dma-buf objects, 0 pools,
0 splats afterwards. The guest doing both jobs at once logged
`@rx_niov_alloc_failed: 450` — the F2 mechanism under genuine pressure — and
still completed and verified every byte.

### N4. RDMA dma-buf during devmem, and the devmem side under it

Covered by N1: the devmem VRAM -> VRAM transfer is unaffected (314572800 bytes,
0 mismatches, 0 linear) by a concurrent RDMA attempt; it is the RDMA side that
cannot get its TCP control connection through.

## 7. State left

**Stack 3 is up and healthy, in the configuration I found it**
(`VE/99-final-state.txt`):

| Item | State |
| --- | --- |
| Guests | r102-1 and r102-2, both `7.2.6-devmemp2p`, 10.102.100.11 / .12, `PORT_ACTIVE` |
| Modules | `rxfrag`: ionic `0FDEA047AB0132E8F7AB9C8`, `ionic_rdma` `24E57766CB83B79E273C7B0`, `ernic_p4` `66D2E2195859E80AE848D07`; amdgpu and `sdxi_char` loaded |
| Servers | `B/ernic/poc-rel` (2d3a3d02 Release) through `verify/hs-wrapper.sh`, `compat + header-split`, header split on, no P4 program |
| Driver settings | restored to as-found: 4 combined channels, `tcp-data-split off`, MTU 1500, rings 1024/1024, offloads on, `tcp_retries2` 15 |
| Leaks | 0 dma-buf objects, 0 dmabuf page pools, 0 pinned amdgpu BOs, `mem_info_vram_used` 2838528 (the baseline) on both guests |
| Kernel logs | **0** lines matching BUG / WARNING / Oops / Call Trace in either guest |
| Emulator | all error counters 0 on both servers (`rx/tx_dma_error`, `rx/tx_desc_fetch_error`, `rx/tx_desc_data_error`, `rx/tx_queue_error`, `frames_tx_bad`) |
| Last checks in this state | `quick-start test` base rc 0, `--rocm-rdma-mode dmabuf` rc 0 and validated, 16 MiB VRAM -> VRAM devmem PASS |
| Stacks 1 and 2 | untouched |
| Added to guest disks | `~/vrfy/vprobe.bt` in both guests (my probe); scratch files under `/tmp` |
| Host | `V/scripts/` and `V/evidence/` (new); `B/evidence/quickstart/VFY-*` (three quick-start runs) |

The stack was cold-cycled twice (never warm-rebooted, never `quick-start up` on
a running stack), both times through `B/scripts/stack.sh`, to run section 3.5
on the compat server and to come back.

## 8. Files

    V/scripts/  venv.sh vpat.py vprobe.bt vcase.sh vbidir.sh vkill.sh
                vrdma-concurrent.sh voff.sh vfrag.sh
    VE/00-as-found-driver-state.txt      the settings I found and restored
    VE/V1-vram2vram.txt (+ -rxprobe, -txprobe)   the headline re-run
    VE/V2-udma-rx.txt, VE/V3-final-smoke.txt
    VE/X2-killrx.txt X3-bidir.txt X4-rdma-during-devmem.txt
       X4b-rdma-control.txt X4c-plain-tcp-under-binding.txt
       X5-offloads-off.txt X6-frag-rxbp.txt X6-frag-rxfrag.txt
       X7-free-while-bound.txt X8-compat-profile.txt
    VE/kernel-*.txt rxbp-*.txt frag-*.txt e2e-*.txt   the source and evidence audits
    VE/VFY-compat-rxmods/                quick-start base on the compat server
    VE/99-final-state.txt                the state the stack was left in
    B/evidence/quickstart/VFY-final-base, VFY-final-rdma-dmabuf
