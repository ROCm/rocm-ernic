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
./fuzz_replay_dhcp_server crash-da39a3ee5e6b4b0d
```

## Harnesses

| Harness | Target | Notes |
|---|---|---|
| `fuzz_rdma_cm_proto` | `rdma_cm_process_message()` | Pure TCP-payload parser — no device state |
| `fuzz_dhcp_server` | `dhcp_server_process()` | DHCP packet parser against a created `DhcpServer` |
| `fuzz_net_headers` | `parse_eth/ip/tcp/udp_header` + checksums | Header-only helpers from `net_headers.h` |

Inputs are copied into an exact-size heap buffer before each call, so
AddressSanitizer flags any read the parser performs past the supplied
length.

## Not yet fuzzed (device-fixture required)

`eth_rx_inject_frame()` and the ionic devcmd and datapath handlers in
`src/ionic_rdma_devcmd.c` and `src/ionic_datapath.c` are **not** fuzzed
here. They are not byte-buffer parsers: they move guest data through
`rdma_pci_dma_map()` and operate on the RDMA resource manager, so a
faithful harness needs a fully-wired device (PCI config + DMA + backend)
rather than a raw buffer. A device-emulation harness that maps a fake DMA
region and drives the admin and doorbell rings is the way to reach them;
that is future work.

## Extending a corpus

Drop representative inputs (a captured DHCP DISCOVER, an rdma_cm SA
message, a real Ethernet/IP/UDP frame) into `corpus/<harness>/` to speed
up coverage. The build seeds each corpus with a single trivial input.
