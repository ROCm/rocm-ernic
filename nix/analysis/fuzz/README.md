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

`eth_rx_inject_frame()` and the pvrdma command handlers in
`hw/rdma/vmw/pvrdma_cmd.c` are **not** fuzzed here. They are not
byte-buffer parsers: they move guest data through `rdma_pci_dma_map()` and
operate on the RDMA resource manager, so a faithful harness needs a
fully-wired `PVRDMADev` (PCI config + DMA + backend) rather than a raw
buffer. A device-emulation harness that maps a fake DMA region and drives
the command ring is the way to reach them; that is future work.

## Extending a corpus

Drop representative inputs (a captured DHCP DISCOVER, an rdma_cm SA
message, a real Ethernet/IP/UDP frame) into `corpus/<harness>/` to speed
up coverage. The build seeds each corpus with a single trivial input.
