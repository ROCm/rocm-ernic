# rocm-ernic emulator: ionic Ethernet TX offloads (SG, CSUM_PARTIAL, TSO)

Branch `feat/devmem-poc`, all uncommitted. Both stages are done; TSO is granted.
Not tested against a real guest: unit and in-process tests only.

## Design

- **Negotiation.** `LIF_SETATTR(FEATURES)` answers `requested & 0x7a0`
  (TX_SG, TX_CSUM, TSO, TSO_IPV6, TSO_ECN) on both the devcmd and adminq paths (one
  handler); LIF identity `eth.config.features` reports the same mask. No RX_CSUM,
  VLAN, tunnel or timestamp bits. Not gated by a profile or option.
- **Q_IDENTIFY / Q_INIT.** TXQ version = `min(cmd.ver, 1)`, `supported = 3`; v1 is
  256-byte SG lists, 15 elements, stride 16; a driver asking v0 still gets 128/8/8.
  Q_INIT stores `ver` per queue (it selects the SG slot size) and refuses a TXQ
  `ver > 1` with `IONIC_RC_EVERSION`.
- **Descriptor.** Opcode, flags, nsge, hword1/2 are decoded from offsets. `len` is the
  first buffer; frame = `len + sum(sg.len)`. All lengths are checked before any data is
  read; nothing is truncated or clamped.
- **CSUM_PARTIAL** (`src/ionic/ionic_eth_tx.c`): `skb_checksum_help()` semantics, 0
  stored as 0xffff. ENCAP is accepted there. CSUM_HW, the VLAN flag, SOT/EOT outside
  TSO and unknown opcodes are refused.
- **TSO.** No state is kept across doorbells. At an opcode-TSO descriptor the device
  gathers the first descriptor, parses the headers (one 802.1Q/ad tag, IPv4 options,
  IPv6 hop-by-hop/dst-opts/routing with 0 segments left, TCP ending exactly at
  `hdr_len`), then scans to EOT checking every descriptor's flags, `hdr_len`/`mss` and
  sizes. If the producer index comes first, the chain waits. Then it re-reads each
  descriptor, re-checks it, and emits one frame per descriptor with headers rewritten
  and IPv4/TCP checksums computed from scratch. One completion per chain,
  `comp_index` = last descriptor, written after all data is read.
- **Bounds.** Frame 9600; `hdr_len <= 256`; `hdr_len + mss <= 9600`; per-descriptor
  payload 1..mss; chain payload <= 65535; nsge <= 8 (v0) / 15 (v1) and only with
  `IONIC_QINIT_F_SG`.
- **Counters.** No new counter: every new reject maps to the existing
  `tx_desc_data_error` / `tx_desc_fetch_error` / `tx_dma_error` (the LIF stats layout
  is the driver's). A refused chain counts once; each emitted segment counts as a frame.

## Files

- New: `src/ionic/ionic_eth_tx.{c,h}`, `tests/test_ionic_eth_tx.c` (94 rows, test
  `ionic-eth-tx-unit`), `tests/golden/ionic_eth_tx_frames.h`.
- Changed: `src/ionic/ionic_eth_emu.c`, `ionic_eth_emu_priv.h`,
  `src/net/net_headers.h`, both `CMakeLists.txt`, `tests/test_ionic_harness.c`,
  `tests/golden/ionic_compat_identity.h` (3 bytes), `tests/py/ernic/eth.py`,
  `tests/py/test_p4.py`, `docs/{ionic,architecture,performance}.rst`, `CHANGELOG.md`.

## Guest-visible changes

- `ethtool -k`: SG, tx-checksum-ip-generic, TSO, TSO6, TSO-ECN on.
- TXQ SG lists are v1 (256 bytes).
- Breaking for hand-written clients: `len` semantics; empty buffers/elements, SG
  without the SG flag, CSUM_HW and the VLAN flag are now refused (still completed).

## Results

| Check | Result |
| --- | --- |
| GCC 13 Debug, `-DERNIC_WERROR=ON -DERNIC_BUILD_P4=ON`, `ctest -j32` | 140/143 pass |
| GCC 13 Release, Clang 18 Debug, WERROR (build) | clean |
| clang-format-18 over all `.c`/`.h` | clean |
| clang-tidy 23.1.4 (`uvx`), on the 4 changed `.c` files, Clang 18 compile DB | clean |
| ASan/UBSan (clang-18), 18 affected tests | pass |
| valgrind memcheck, 7 affected tests | pass, 0 errors |
| Coverage (clang-18) | `src/` 91.60% lines (floor 90); `ionic_eth_tx.c` 100% from the new test |
| black, pylint, vermin, codespell, doc8, cmakelint | clean |
| Revert check (HEAD's `ionic_eth_emu.c`) | 82 of 94 new rows fail; harness eth, eth-dma and `ernic-p4-driver` fail |
| Golden frames | checksums and segmentation re-derived with scapy: match |

The 3 failing ctest tests fail for reasons outside this change: `ernic-socket-unit`
(ACL check on `/tmp` returns EIO; links no ionic code), `ernic-ci-images` and
`ernic-check-guest-image-tag` (their temp-repo `git commit` hits the GPG signing and
pre-commit hooks). I did not confirm they fail on a clean tree.

The 12 rows that pass against old code are guards on unchanged behaviour (plain frame,
oversize, zero `len`, 9 SGEs on v0, DMA faults, "grant nothing", Rx queue).

## Not done / caveats

- IWYU not run (not installed); includes reviewed by hand.
- clang-tidy ran standalone on changed files, not as the `ERNIC_USE_CLANG_TIDY` build,
  and with Clang 18's flags rather than Clang 23's. GCC 15 / Clang 21 not available.
- Sphinx docs build fails on one warning in `docs/api.rst` (duplicate
  `rocm_ernic_dev_t`), in files this change does not touch; no warning from the edited
  pages.
- TSan not run: no shared state or threads changed.
- A data-DMA failure, or a guest rewriting a descriptor between check and send, stops
  a chain part-way with earlier segments already sent (counted once).
- TSO over tunnels, VLAN insertion, AccECN, IPv6 fragment/AH/ESP headers: refused.
- `docs/performance.rst` TCP figure not re-measured.
- Scratch: `/var/tmp/ionic-devmem/test/emu-golden/` (scapy check),
  `/var/tmp/ionic-devmem/test/ernic-revert/` (revert tree).

## Review round

14 findings (5 and 14 are the same). All verified real; none rejected or deferred.

1. **Tx SG list address may wrap** - FIXED. `eth_tx_sizes()` refuses
   `sg_ring_base > UINT64_MAX - sg_off` as a fetch error, as `eth_rx_bufs()` does.
   Test `check_sg_ring_wraps()` plants a usable list at the wrapped address.
2. **Unfinished TSO chain re-gathers and re-reads SG lists every doorbell** -
   FIXED. New `eth_tx_tso_end()` finds the chain's end from descriptors alone and
   returns before anything else is read. A wait still costs one 16-byte read per
   posted descriptor per doorbell (as single frames do); no state is kept across
   doorbells. Stated in the comment and `docs/ionic.rst`. Side effect: a chain cut
   off by an unreadable or foreign descriptor is refused without gathering its
   first descriptor.
3. **65535 bound only in the check pass** - FIXED. `eth_tx_tso_send()` re-checks
   it; re-read and re-check share `eth_tx_tso_next()`. Test: a chain rewritten
   mid-send from 57376 to 66822 bytes sends six segments, not seven.
4. **TSO6 shapes Linux posts and the device refuses** - FIXED in words (confirmed
   `net/core/dev.c:3895-3900` keys on `NETIF_F_IPV6_CSUM`). Docs note and
   CHANGELOG: dropped silently, only `tx_desc_data_error`, workaround
   `ethtool -K <dev> tx-tcp6-segmentation off`. Segments-left != 0 stays refused.
5. / 14. **`eth.py` generic SG constants** - FIXED: `ETH_TXQ_SG_*`,
   `ETH_RXQ_SG_STRIDE`, `sg_stride(qtype)`.
6. **`nsge != 0` with `sg_ring_base == 0`** - FIXED at Q_INIT: `q->sg` needs the
   flag and a non-zero base, so Rx SG descriptors get it too. Test
   `check_sg_ring_absent()`; CHANGELOG and docs updated.
7. **`hdr_len <= 256` unpinned** - FIXED. Golden chain with 254 bytes of headers
   (the largest reachable) accepted; 262 refused.
8. **Vacuous no-payload row** - FIXED: single descriptor, mss 1000; plus
   `check_tso_short_first()` (69 of 70 header bytes, stale headers in the buffer).
9. **One-byte ethertype pokes** - FIXED: both bytes; reverse row; two rows valid
   but for the version nibble, which are what kill the version checks.
10. **Untested accepted forms** - FIXED: golden chains for 802.1ad and dst-opts +
    routing (0 segments left); RST row.
11. **Loosenable TSO checks** - FIXED: EOT set/cleared mid-send, payload exactly
    65535/65536, first descriptor mss + 1, ENCAP on a later descriptor,
    offset-only fragment.
12. **Docs overstate "not sent in part"** - FIXED: new paragraph on chains that
    stop part-way.
13. **`ionic_eth_tx.c` missing from architecture table** - FIXED.

New golden frames: scapy built each segment
(`/var/tmp/ionic-devmem/test/emu-golden/gen_review.py`), TCP checksums recomputed
by hand in the script; `verify.py` passes all 34 arrays.

### Re-run results

| Check | Result |
| --- | --- |
| GCC 13 Debug, WERROR, P4, `ctest -j` | 140/143; same 3 unrelated failures |
| `ionic-eth-tx-unit` | 112 rows (was 94), pass |
| 19 mutants of the two `.c` files (`test/emu-mutate/mutate.py`) | each fails 1-2 rows; `hdr_len` bound removed aborts under ASan |
| GCC 13 Release, Clang 18 Debug, WERROR (build) | clean |
| clang-format-18, all `.c`/`.h` | clean |
| clang-tidy 23.1.4 (`uvx`), 4 `.c` files, Clang 18 compile DB | clean after fixing 2 findings of mine |
| ASan/UBSan (clang-18), `-R 'ionic|p4'` | 87/87 |
| valgrind, 6 ionic unit + 5 harness tests | pass, 0 errors in 11 logs |
| Coverage (clang-18), fresh profiles | `src/` 91.68% lines (floor 90); `ionic_eth_tx.c` 100%, `ionic_eth_emu.c` 94.34% |
| black, pylint, vermin (`eth.py`), codespell, doc8 | clean |
| Sphinx | only the pre-existing `docs/api.rst` warning |

Coverage build only: harness tests time out at random under `ctest -j` (3 in the
full run, including ones with no Ethernet Tx); each passes alone in 0.02 s. Hung
processes sit in `nfs4_retry_setlk`, the profile file's lock on an NFS home. Not
seen in the GCC, ASan or valgrind runs; not checked against the tree before this
round.

Not run: IWYU (no `#include` changed), TSan (no shared state), GCC 15 / Clang
21, clang-tidy as the `ERNIC_USE_CLANG_TIDY` build, any guest. Finding 4's
retransmit behaviour is reasoned, not observed.
