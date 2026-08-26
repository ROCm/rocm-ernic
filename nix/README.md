# Nix flake: dev shell, build, and analysis toolkit

This flake provides a reproducible development environment, a hermetic
build of rocm-ernic, and a suite of static, dynamic, and fuzz analysis
targets. The design is modular: a slim top-level `flake.nix` wires together
small single-purpose modules under `nix/`.

> Flakes only see git-tracked files. After adding or editing files under
> `nix/`, `git add` them before `nix build` / `nix develop`.

## Dev shell & build

```
nix develop                 # dev shell (gcc, cmake, analysis tools); 'ernic-help'
nix build .#rocm-ernic      # -> ./result/bin/rocm-ernic
nix build .#libvfio-user    # the from-source dependency (not in nixpkgs)
```

In the dev shell: `ernic-configure`, `ernic-build`, `ernic-test`,
`ernic-clean`. The build is pinned to gcc to match the project's CI.

## Static analysis

Each tool is a build target; aggregates also run the triage prioritiser.

```
nix build .#analysis-quick       # clang-tidy + cppcheck + triage
nix build .#analysis-standard    # + flawfinder, clang-analyzer, gcc-warnings
nix build .#analysis-deep        # + gcc-analyzer, semgrep
cat result/summary.txt
cat result/triage/high-confidence.txt
```

Per-tool targets: `analysis-clang-tidy`, `analysis-cppcheck`,
`analysis-flawfinder`, `analysis-semgrep`, `analysis-gcc-warnings`,
`analysis-gcc-analyzer`, `analysis-clang-analyzer`, and `compile-db`
(the shared `compile_commands.json`).

Triage (`nix/analysis/triage/`) loads every tool's report, drops noise and
out-of-scope paths, deduplicates, cross-references findings flagged by
multiple tools, and ranks by priority. `src/from-qemu/utils/` and
`hw/rdma/` (the untrusted-input parsers) are treated as security-sensitive.

## Dynamic analysis

Build-and-exercise on the loopback backend (no RDMA hardware):

```
nix build .#analysis-sanitizers  # ASan + LSan + UBSan
nix build .#analysis-tsan        # ThreadSanitizer
nix build .#analysis-valgrind    # memcheck (independent leak detector)
cat result/summary.txt
```

Each starts the server, runs the PCI-config test client, then shuts the
server down cleanly so leak checkers report at exit.

## Fuzzing

```
nix build .#fuzz            # build harnesses + corpora + run-fuzzers
FUZZ_TIME=300 ./result/bin/run-fuzzers   # long campaign, crashes to a temp dir

nix build .#fuzz-run        # bounded run in-sandbox, collect crashes
cat result/summary.txt
cat result/crashes/*/repro.txt
```

Harnesses (`-fsanitize=fuzzer,address,undefined`) target the wire parsers:
`rdma_cm_process_message`, `dhcp_server_process`, and the `net_headers.h`
parse/checksum helpers. See `nix/analysis/fuzz/README.md` for details and
for the DMA-path targets deferred to a future device-fixture harness.

## Module map

| Path | Purpose |
|---|---|
| `flake.nix` | inputs + per-system outputs |
| `nix/libvfio-user.nix` | from-source libvfio-user (+ synthesized `.pc`) |
| `nix/packages.nix` | dependency / tool sets |
| `nix/derivation.nix` | CMake build of rocm-ernic |
| `nix/devshell.nix`, `nix/shell-functions/` | dev shell + helpers |
| `nix/compat-cflags.nix` | gcc-15 C99-error downgrade (build + shell) |
| `nix/analysis/` | static + dynamic + fuzz analysis modules |
| `nix/analysis/triage/` | finding prioritiser (Python) |
