# rocm-ernic emulator: ionic RX header split + RX checksum (opt-in mode)

Tree: `/var/tmp/ionic-devmem/rocm-ernic-rx` (branch `feat/devmem-poc`, all uncommitted, on
top of the uncommitted TX-offload work). `~/Projects/rocm-ernic` was not
edited. Spec: `rx-interface.md`; nothing in it was unimplementable, and its
"Changes during implementation" has five clarifications from this work (no rule changed).
Not run against a guest: unit, in-process and server-start tests only.

Patch for merging back (RX work only, relative to the home tree's working copy, 21 files;
`git apply --check` in the home tree passes): `/var/tmp/ionic-devmem/patches/emulator-rx-hds.patch`.

## Design

- **Mode.** New profile section/key `[rx] header_split = off|on`; `compat` sets `off`; new
  built-in profile `header-split` (only that key). Server option: `--nic-profile header-split`.
  No dedicated flag, no environment variable. Fixed for the life of the process.
- **Frame parsing** is a new DMA-free module, `src/ionic/ionic_eth_rx.{c,h}`:
  `ionic_eth_rx_parse(frame, len, &info)` gives `csum` (K2), `csum_flags` (K2-K4),
  `splittable` and `hdr_len` (P1). Every length is checked against `len` before use
  (overflow-safe: `x > len - off` forms); offsets come from `net/net_headers.h` (UDP header
  offsets added there) and `byteorder.h` accessors. It is called once per frame, and only
  with the mode on.
- **Placement** (`eth_rx_bufs()` in `ionic_eth_emu.c`): takes the header length of a
  splittable frame on an HDS queue (0 otherwise) and returns the buffers plus `hds_flags`.
  Split = SG opcode, `hdr_len <= desc.len`, payload fits the SG elements up to the first
  zero-length one; then buffer 0 is gathered as exactly `hdr_len` bytes. Otherwise today's
  prefix fill, with `HBO` iff only the buffer-0 size check failed. Nothing is written until
  the whole frame is known to fit (unchanged rule).
- **Completion** (`eth_rx_deliver()`): HDS queue = 32-byte entry at `cq_ring_base + i*32`,
  extension at 0 (`hds_hdr_len` le16 @0, `hds_flags` @2, rest zero), base completion at 16;
  other queues 16 bytes as before. Mode on: `csum` written big-endian at @8/@9, `csum_flags`
  @14, on every RX queue. Mode off: both zero, nothing parsed.
- **Negotiation.** `Q_IDENTIFY(RXQ).features = 0x20020` in mode on (other qtypes 0, `comp_sz`
  16). `Q_INIT(RXQ)` in mode on reads `features` @56: bits outside `0x20020` -> `ENOSUPP`;
  half a pair, or HDS without `F_SG`/non-zero `sg_ring_base` -> `EINVAL`; existing checks
  first; a refusal leaves the queue untouched. `struct eth_queue` gains `hds`. LIF features:
  `0x7a0`, plus `RX_CSUM` (`0x7b0`) in mode on, in LIF identity and `LIF_SETATTR`.
- **Mode off** reads no new field and writes no new byte; `Q_INIT` still ignores `features`.

## Files

- New: `src/ionic/ionic_eth_rx.{c,h}`, `profiles/header-split.ini`,
  `tests/test_ionic_eth_rx.c` (test `ionic-eth-rx-unit`), `tests/golden/ionic_eth_rx_frames.h`.
- Changed: `src/ionic/ionic_eth_emu.c`, `ionic_eth_emu_priv.h`, `ionic_profile.{c,h}`,
  `ionic_profiles.c`, `src/net/net_headers.h`, `src/rocm_ernic_server.c` (one startup line,
  mode on only), `profiles/compat.ini`, both `CMakeLists.txt`, `tests/test_ionic_profile.c`,
  `tests/test_ionic_ci.sh` (Test 6c), `nix/analysis/fuzz/fuzz_profile_parse.c` (3 seeds),
  `docs/ionic.rst` (new "Receive header split" section, profile tables),
  `docs/architecture.rst`, `CHANGELOG.md` (two Added entries).
- Outside the tree: `test/scripts/stack.sh` gained `server-args` (below);
  `test/emu-golden-rx/{gen_rx.py,verify_rx.py}` (golden frames), `test/emu-rx-mutate/mutate.py`.

## Behaviour notes

- Not guest-visible but changed in both modes: `--print-profile` prints `[rx]`, and the
  profile hash (startup log, stats file `nic_profile`) differs from before for every profile.
- A splittable frame with payload always reads the SG list, so an unreadable list drops it
  (`rx_desc_fetch_error`) even if it would have fitted buffer 0; a headers-only frame reads
  no list.
- `tests/golden/ionic_compat_identity.h` and all existing tests are unchanged and pass.

## Stack binary and switching the mode on

- Binary: `/var/tmp/ionic-devmem/test/ernic/rx-hds-1/build/rocm-ernic`
  (sha256 `f6e9b701...d457768`, built by `ERNIC_REPO=/var/tmp/ionic-devmem/rocm-ernic-rx
  scripts/build-ernic.sh rx-hds-1` in the stack image, linked to the image's libvfio-user).
- Mode: server option `--nic-profile header-split` (profile key `[rx] header_split = on`).
  qemu-tool's gen-compose has no pass-through for server options, so `stack.sh` now has:
  `stack.sh server /var/tmp/ionic-devmem/test/ernic/rx-hds-1/build/rocm-ernic` then
  `stack.sh server-args --nic-profile header-split` (`server-args none` to undo). It mounts
  the binary at `/opt/rocm-ernic-local/rocm-ernic` and a generated wrapper over
  `/usr/local/bin/rocm-ernic` that appends the options. With no `stack/server-args` file the
  override is byte-identical to before. Both subcommands restart the stack; **I ran neither**
  (the stack was up and in use). Confirm in the ernic log: `NIC profile: compat + header-split`
  and `NIC Rx header split: on`.

## Results (evidence: `/var/tmp/ionic-devmem/test/evidence-emu-rx/`)

| Check | Result | Evidence |
| --- | --- | --- |
| GCC 13 Debug, `-DERNIC_WERROR=ON -DERNIC_BUILD_P4=ON`, `ctest -j32` | PASS 141/144; 3 fail for host reasons (below) | `ctest-gcc-debug.txt`, `build-gcc-debug.txt` |
| `ionic-eth-rx-unit` (220 rows) | PASS, also in GCC Release and Clang builds | `ionic-eth-rx-unit.txt` |
| 60 mutants of `ionic_eth_rx.c` / `ionic_eth_emu.c` (ASan build) | PASS: all killed | `mutants.txt` |
| Golden frames re-derived with scapy 2.8.0 | PASS 50/50 | `golden-verify.txt` |
| GCC 13 Release, Clang 18 Debug, WERROR (build) | PASS, 0 warnings | `build-gcc-release.txt`, `build-clang-debug.txt` |
| clang-format-18, every `.c`/`.h` | PASS | `clang-format.txt` |
| clang-tidy 23.1.4 (`uvx`), 8 changed `.c` files, Clang 18 compile DB | PASS after fixing 7 findings in my tests | `clang-tidy.txt` |
| ASan/UBSan (clang-18), `ctest -R 'ionic\|p4'` | PASS 88/88, no report in the log | `ctest-asan.txt` |
| valgrind memcheck, 7 unit tests (rx, tx, profile, identity, queues, stats) | PASS, 0 errors in 7 logs | `ctest-valgrind.txt` |
| valgrind on the server, 5 server tests incl. `ionic-ci` | PASS, 0 errors in 59 logs | `ctest-valgrind-server.txt` |
| Coverage (clang-18) | `src/` 92.59% lines (floor 90); `ionic_eth_rx.c` 100% | `build-coverage.txt`, `ctest-coverage.txt` |
| ShellCheck (0.9.0, not CI's 0.11.0), codespell, doc8, cmakelint | PASS | `lint.txt` |
| Sphinx docs build | builds; one warning, in `docs/api.rst`, untouched | `docs-build.txt` |
| Stack binary: `--print-profile` in the stack image, plain and through the wrapper | PASS | `stack-binary-check.txt`, `build-ernic-rx-hds-1.txt` |
| Guest run (stock or HDS driver), K2 byte order against a kernel | NOT RUN | |
| `stack.sh server-args` against the live stack | NOT RUN (only its override output, in a scratch copy) | |
| TSan, IWYU, GCC 15 / Clang 21, `ERNIC_USE_CLANG_TIDY` build, ansible checks | NOT RUN | |

The 3 failing ctest tests are the same as in `impl-emulator.md`: `ernic-socket-unit` (socket
mode/ACL check in `/tmp` on this host) and `ernic-ci-images`, `ernic-check-guest-image-tag`
(their temp-repo `git commit` is blocked by the global pre-commit hook and GPG signing).

Test design: every row compares all 16 KiB of fake guest memory with an expected image
(frame bytes per buffer, one completion, nothing else touched). 50 frame kinds are each run
on an HDS queue, a non-HDS queue in mode on, and mode off (150 rows); 25 buffer-geometry
rows; the spec's examples A and B byte for byte at ring entries 5 and 6; ring wrap; stale
frame-buffer bytes; every prefix of every frame in an exact-size buffer (for ASan/valgrind);
DMA faults; LIF features, `Q_IDENTIFY`, `Q_INIT` rows. The mode-off rows are guards on
unchanged behaviour: they could not be run against the pre-change tree (it has no profile
key to compile against); the mutants "features read in mode off", "RX_CSUM always granted"
and "Q_IDENTIFY features in mode off" show they do fail when mode off is broken.

## Leftovers

- No fuzz target for `ionic_eth_rx_parse()` (the prefix sweep under ASan stands in).
- No Python harness (`tests/py`) or `test_ionic_harness.c` row drives the mode through the
  real vfio-user path; only the unit test and the server-start check do.
- The scapy check of the golden frames is a manual step (`test/emu-golden-rx/`), not in CI.
- `HBO` cannot be observed with a `SIMPLE` descriptor by construction (it would not fit).
- Scratch left in place: `test/emu-rx-mutate/work/` (snapshot + build),
  `test/scratch/rx-stack-dry/`, build directories `build-*` inside the RX tree (git-ignored),
  `test/evidence-emu-rx/stack.sh.before` (the script before my change).

## Review round 2 (2026-11-19; `test-rx.md` "Review round", findings 1 and 2)

- `Q_INIT` refusal log: the `features=` value and the "Rx features 0 or 0x20020 with an SG
  ring" clause are logged only for an Rx queue with header split on, where the features are
  read; every other refusal logs the line without them. Test: `check_q_init_refusal_log()` in
  `tests/test_ionic_eth_rx.c` (failed first: 2 rows), with `g_last_warning` added to
  `tests/ionic_eth_emu_fakes.h`.
- No behaviour change for the SG list of a splittable frame that would fit `buf0`: it is
  still read and an unreadable list still drops the frame. Now documented in `docs/ionic.rst`
  ("What is split"), in the comment on `eth_rx_bufs()` and in `rx-interface.md` P5.
- Checks run: clang-format-18 (all files), GCC 13 Debug with WERROR and ctest (141/144; the
  same 3 host-specific failures), GCC 13 Release and Clang 18 Debug builds, ASan
  `ctest -R ionic-...` 62/62, clang-tidy 23.1.4 on `ionic_eth_emu.c` and the 5 tests that
  include the fake, codespell, doc8, Sphinx (the one old `api.rst` warning). Evidence:
  `test/evidence-emu-rx/review/`. Not run: TSan, valgrind, coverage, IWYU.
- Stack binary: `test-rx/ernic/rx-hds-2/build/rocm-ernic` (sha256 `8dbed350...448ad23d`), in
  use by stack 2 in both modes during the review round.
- No CHANGELOG entry: the existing header-split entry does not describe the log line.
