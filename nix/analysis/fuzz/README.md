# Fuzz harnesses

libFuzzer harnesses for rocm-ernic's untrusted wire-format parsers. Built
and run via Nix:

```
nix build .#fuzz          # build the fuzzer binaries + seed corpora
./result/bin/run-fuzzers  # run all harnesses (FUZZ_TIME=<secs>, default 60)

nix build .#fuzz-run      # build + bounded run in the sandbox, collect crashes
cat result/summary.txt
```

Each harness is built with `-fsanitize=fuzzer,address,undefined`, so
out-of-bounds accesses and undefined behaviour abort with a diagnostic.

## Staying buildable between fuzz runs

Because the Nix targets above are opt-in, nothing on an ordinary PR used to
compile these harnesses — a changed parser signature could break them and
stay broken until someone asked for a fuzz run.

`tests/nix/` therefore builds every harness as part of the normal CMake
build, linked against a plain `main()` (`tests/nix/fuzz_replay_main.c`)
instead of libFuzzer, and registers each as a `fuzz-replay-<harness>` test:

```
ctest -R fuzz-replay
```

The compile is what catches the drift; the replay over a few generated
inputs is a cheap smoke test on top. `-fsanitize=fuzzer` is clang-only, but
the driver needs no libFuzzer, so this builds under gcc too. The harnesses
are held to the full project warning set there — they are our code, not
vendored, and are warning-clean.

**When adding a harness, add it in both places**: the `harnesses` list in
`nix/analysis/fuzz.nix` and the `ernic_add_fuzz_replay()` calls in
`tests/nix/CMakeLists.txt`. One without the other leaves it compiled only
by the opt-in workflow again.

The driver also replays a saved crash input, which reproduces a finding
from a real fuzz run under ASan without rebuilding via Nix:

```
./fuzz_replay_rdma_cm_proto crash-da39a3ee5e6b4b0d
```

## Harnesses

| Harness | Target | Notes |
|---|---|---|
| `fuzz_rdma_cm_proto` | `rdma_cm_process_message()` | Pure TCP-payload parser — no device state |
| `fuzz_net_headers` | `parse_eth/ip/tcp/udp_header` + checksums | Header-only helpers from `net_headers.h` |
| `fuzz_ionic_stats` | ionic `STATS_HDRS` / `STATS_VALS` / `QP_STATS_HDRS` / `QP_STATS_VALS` via `dispatch_wqe()` | Fake device from `tests/ionic_adminq_fakes.h`, shared with `tests/test_ionic_stats.c`; checks every result against the handler's promises, with and without an injected `stats_vals` fault |
| `fuzz_fault_parse` | `ionic_fault_parse()`, the `--inject` spec parser | Pure string parser; checks an accepted spec is self-consistent and round-trips through its canonical form |

Inputs are copied into an exact-size heap buffer before each call, so
AddressSanitizer flags any read the parser performs past the supplied
length.

## The ionic stats harness

`fuzz_ionic_stats` is the first harness that reaches a device handler. It
`#include`s `src/ionic_adminq.c` and posts one of the four stats opcodes
through `dispatch_wqe()`, with guest memory and the counter snapshots
supplied by `tests/ionic_adminq_fakes.h` -- the same fake device the unit
test uses, so the table and the fuzzer can't drift apart. Beyond ASan, it
aborts when a result breaks one of the handler's promises: one DMA write at
the buffer the body named, never past its length, in whole entries, with
the NONE terminator when it fits, the right counters in header order, no
QP values for an unmapped QP, and nothing written on failure or outside
the write. The status must also be the code its cause names (`EINVAL` for
a short body, `BAD_ADDR` for a zero `dma_addr`, `ENOENT` for an unmapped
QP), and an input can switch on an injected `stats_vals` fault, under which
device values must fail with `EIO` and every other command is unaffected.

`fuzz_fault_parse` drives `ionic_fault_parse()` with arbitrary strings. A
rejected spec must leave a message and own nothing; an accepted one must
be self-consistent, parse back from its canonical form to the same fault,
and count its events exactly. It defines the same seed table and
`FUZZ_EMIT_SEEDS` hook as the stats harness.

`LLVMFuzzerInitialize()` first runs a table of seed inputs (positive,
negative, boundary and corner rows, each with a description and expected
outcome) through the same checks, and aborts if any fails, so a broken
oracle can't fuzz quietly. With `FUZZ_EMIT_SEEDS=<dir>` it also writes the
rows out; `fuzz.nix` does that to build the harness's seed corpus.

## Not yet fuzzed (device-fixture required)

The rest of the ionic devcmd and datapath handlers in
`src/ionic_adminq.c`, `src/ionic_rdma_devcmd.c` and `src/ionic_datapath.c`
are **not** fuzzed yet. They create and modify resources in the RDMA
resource manager and walk the guest's rings, so they need more of the
device faked than the stats commands do. Extending
`tests/ionic_adminq_fakes.h` towards them is the natural next step.

## Extending a corpus

Drop representative inputs (an rdma_cm SA message, a real
Ethernet/IP/UDP frame) into `corpus/<harness>/` to speed
up coverage. The build seeds each corpus with a single trivial input.
