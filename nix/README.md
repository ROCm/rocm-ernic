# Nix flake: dev shell, build, and analysis toolkit

This flake provides a reproducible development environment, a hermetic
build of rocm-ernic, and a suite of static, dynamic, and fuzz analysis
targets — the same toolchain and checks the maintainers use, pinned so you
get identical results. The design is modular: a slim top-level `flake.nix`
wires together small single-purpose modules under `nix/`.

## New to Nix?

[Nix](https://nixos.org) is a package manager that builds software in
isolation from pinned inputs. In practice that means `nix develop` drops you
into a shell with the exact compilers and tools this project needs — nothing
installed system-wide, nothing to conflict with your distro — and `nix build`
produces the identical binary on any machine.

**Install Nix** (multi-user, recommended):

```
sh <(curl -L https://nixos.org/nix/install) --daemon
```

Single-user (no root, e.g. in a container): use `--no-daemon` instead. Full
instructions: <https://nix.dev/install-nix>.

**Enable flakes** (once). Either add this line to `/etc/nix/nix.conf` (or
`~/.config/nix/nix.conf`):

```
experimental-features = nix-command flakes
```

…or prefix each command with
`--extra-experimental-features 'nix-command flakes'`.

**Two commands to get started**, from the repo root:

```
nix develop                 # enter the dev shell; type 'ernic-help'
nix build .#rocm-ernic      # build the server -> ./result/bin/rocm-ernic
```

Video walkthroughs of the install:
[Ubuntu](https://youtu.be/cb7BBZLhuUY) ·
[Fedora](https://youtu.be/RvaTxMa4IiY). Handy references: the
[flakes wiki](https://nixos.wiki/wiki/flakes) and
[search.nixos.org](https://search.nixos.org) to find any package.

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
multiple tools, and ranks by priority. `src/net/`, `src/rdma/` and
`src/qemu/` (the untrusted-input paths) are treated as security-sensitive.

`analysis-semgrep` first runs `semgrep --test` on each rule fixture in
`nix/analysis/semgrep-tests/`, where a `// ruleid:` comment marks a line
the rule must report, a `// ok:` comment one it must not, and a
`// todoruleid:` comment one it should report but can't yet. It fails if a
fixture fails, if a rule doesn't parse, or if the scan fails, scans
nothing, or produces malformed JSON. semgrep's partial-parsing warnings,
for source it could only partly parse, don't fail it. Findings in that
source may be missing, though, so `coverage.txt`, the top of `report.txt`,
and the aggregate targets' `summary.txt` report the warnings.

## Dynamic analysis

Build-and-exercise on the loopback backend (no RDMA hardware):

```
nix build .#analysis-sanitizers  # ASan + LSan + UBSan
nix build .#analysis-tsan        # ThreadSanitizer
nix build .#analysis-valgrind    # memcheck (independent leak detector)
cat result/summary.txt
```

Each starts the server, runs the PCI-config test client, waits for the
server to handle its connection, then shuts the server down cleanly so
leak checkers report at exit. The first line of `summary.txt` says whether
that exercise completed: `exercise: clean`, or
`exercise: incomplete (<reason>)` if the server never started listening,
the client failed, the server never handled the connection, or the server
didn't shut down cleanly. An incomplete exercise fails the build, since
the leak checkers never saw a clean exit; sanitizer and valgrind findings
don't. Neither does an incomplete exercise that reported a sanitizer
finding: the first ASan or UBSan error stops the server, so such a run
can't finish, but its report is real.

`nix build .#checks.x86_64-linux.analysis-runners` tests the exercise
driver against a fake server and client that fail in each of those ways,
and the semgrep report check against malformed and incomplete scans. It
builds no part of rocm-ernic, so the Nix Check workflow runs it on every
change under `nix/`.

## Fuzzing

```
nix build .#fuzz            # build harnesses + corpora + run-fuzzers
FUZZ_TIME=300 ./result/bin/run-fuzzers   # long campaign, crashes to a temp dir

nix build .#fuzz-run        # bounded run in-sandbox, collect crashes
cat result/summary.txt
cat result/crashes/*/repro.txt
```

Harnesses (`-fsanitize=fuzzer,address,undefined`) target the parsers of
untrusted input, such as the ionic stats admin commands and the
`--nic-profile` and `--inject` parsers. See `nix/analysis/fuzz/README.md`
for the full list and for the DMA-path targets deferred to a future
device-fixture harness.

## Cross-compilation & other architectures

From an **x86_64-linux** host the flake also cross-compiles the server for
four target architectures and can boot each under emulation to exercise it.

```
# Cross-build the server (produces an <arch> ELF + the test client):
nix build .#rocm-ernic-aarch64     # ARM64
nix build .#rocm-ernic-riscv64     # RISC-V 64
nix build .#rocm-ernic-armv7l      # 32-bit ARM
nix build .#rocm-ernic-ppc64le     # POWER9 little-endian
file result/bin/rocm-ernic

# Exercise the cross-built server under emulation:
nix run .#run-aarch64-tests        # aarch64/riscv64: build+boot a VM, watch the verdict
nix run .#run-armv7l-tests         # armv7l/ppc64le:  qemu-user smoke of the cross binary
nix build .#microvm-aarch64        # (VM arches) just build the VM; ./result/bin/microvm-run to boot
```

Every arch exposes a uniform `run-<arch>-tests` target, but there are two
run strategies (selected by `runMode` in `nix/microvms/constants.nix`):

- **Full VM** (`x86_64`, `aarch64`, `riscv64`) — `run-<arch>-tests` boots a
  minimal NixOS **microvm** (via
  [microvm.nix](https://github.com/astro/microvm.nix)), runs the server
  against the PCI-config test client on the loopback backend *inside the
  guest*, and gates on `ERNIC Self-Test: SUCCESS`. These arches also expose
  `microvm-<arch>` (`./result/bin/microvm-run` boots it directly). `x86_64`
  runs under KVM as a fast baseline for the harness itself; `aarch64`/
  `riscv64` run under TCG.
- **qemu-user smoke** (`armv7l`, `ppc64le`) — `run-<arch>-tests` runs the
  cross-built server directly under **user-mode** QEMU (`qemu-arm`,
  `qemu-ppc64le`) and asserts it reaches `--help` and exits 0. This still
  exercises the whole target-arch runtime path (dynamic loader, every shared
  library in the closure, the program's own startup), just without a booted
  guest.

**Why the split.** A full NixOS microvm won't boot for `armv7l`/`ppc64le`
here: microvm.nix's QEMU runner derives the system-emulator binary name from
the nix-system prefix (`qemu-system-armv7l`, `qemu-system-powerpc64le`), but
QEMU ships those as `qemu-system-arm` / `qemu-system-ppc64`, so `microvm-run`
fails before the guest starts. `x86_64`/`aarch64`/`riscv64` happen to match
QEMU's names and boot fine. Working around it would mean a full per-arch
QEMU rebuild or disabling microvm's `optimize.enable` (perturbing the boot
proven for the other arches); the qemu-user smoke is the pragmatic
"land what's green" path. Full-VM boots for these two are a follow-up.

### Tested & verified

All targets below were built and run end-to-end from an **x86_64-linux**
host. Each cross-build was confirmed to produce the correct target ELF, and
each run was driven through its `run-<arch>-tests` target to a green verdict.

| Arch | Cross-build — ELF machine (`file`) | Run mode | Run result |
|---|---|---|---|
| `x86_64` | (native x86-64) | full VM, **KVM** | boots; in-guest self-test → `ERNIC Self-Test: SUCCESS`; runner exit 0 |
| `aarch64` | ELF 64-bit LSB, **ARM aarch64** | full VM, **TCG** | boots; in-guest self-test → `SUCCESS`; runner exit 0 |
| `riscv64` | ELF 64-bit LSB, **UCB RISC-V** (OpenSBI `-bios default`) | full VM, **TCG** | boots; in-guest self-test → `SUCCESS`; runner exit 0 |
| `armv7l` | ELF 32-bit LSB PIE, **ARM EABI5** (interp `ld-linux-armhf.so.3`) | qemu-user smoke (`qemu-arm`) | `rocm-ernic --help` runs under emulation, exit 0 → `SUCCESS` |
| `ppc64le` | ELF 64-bit LSB PIE, **PowerPC OpenPOWER ELF V2 ABI** (interp `ld64.so.2`) | qemu-user smoke (`qemu-ppc64le`) | `rocm-ernic --help` runs under emulation, exit 0 → `SUCCESS` |

**What the full-VM run actually checks** (`x86_64` / `aarch64` / `riscv64`):
the guest boots a minimal NixOS microvm, starts the cross-built server on the
loopback backend, and runs the PCI-config **test client** against it. The
client validates the emulated device's config space — Vendor `0x1dd8`
(Pensando), Device `0x1002` (ROCm ERNIC) — and prints `✓ Test PASSED`; the self-test
service then emits `ERNIC Self-Test: SUCCESS` on the serial console, which the
host-side lifecycle driver (`nix/microvms/lib.nix`) matches to exit 0.

**What the qemu-user smoke checks** (`armv7l` / `ppc64le`): the cross binary
is executed directly under user-mode QEMU. That loads the target-arch dynamic
linker and every shared library in the closure (glib, json_c, libvfio-user,
rdma-core, …), runs the program's own startup and argument parsing, and
requires it to reach `--help` and exit 0. It does **not** stand up the
server↔client loopback exercise (no booted guest), so it is a weaker signal
than the full-VM path — enough to prove the cross binary is well-formed and
runnable on the target ABI, not that the full runtime data path works.

**Caveats.** Non-x86 targets run under QEMU **TCG** (full software
emulation), so both the cross-build and the run are **slow** and rebuild a
seccomp-less QEMU the first time. Cross-**build** support is solid for all
four arches. On a native aarch64 host the default `nix build` already
produces an ARM64 binary — the cross targets are for building *from* x86_64.

## Container images (OCI)

The flake builds OCI images straight from the Nix closure (no Dockerfile) via
`dockerTools.streamLayeredImage` — reproducible, content-addressed layers that
dedupe across rebuilds. See `nix/oci.nix`.

```
# amd64 (native host):
nix build .#oci-image          # minimal (distroless): server + closure only
nix build .#oci-image-debug    # + bash/coreutils for `docker exec` debugging

# arm64 / riscv64 (cross, from an x86_64 host):
nix build .#oci-image-aarch64          # also: -aarch64-debug
nix build .#oci-image-riscv64          # also: -riscv64-debug

# streamLayeredImage → the result is a loader script:
./result | docker load
docker run --rm -v "$PWD/sock:/run/rocm-ernic" rocm-ernic:<tag>
```

Minimal images are tagged `rocm-ernic:<version>`; debug images
`rocm-ernic:<version>-debug`. The entrypoint is the server; the default command
uses the self-contained `loopback` backend with the socket under
`/run/rocm-ernic` — override at runtime, e.g.
`docker run … rocm-ernic:<tag> --backend tcp:manager:listen:5000`.

**Deployment note.** rocm-ernic and QEMU rendezvous over a **UNIX socket** (the
`vfio-user-pci` transport), so a container must share that socket path with
whoever attaches to it — bind-mount the socket directory. The socket is mode
`0600`, so only root and the server's user can connect; to let a host QEMU
that runs as another user in, pass `--socket-group` the numeric gid of a group
it's in (group names in the image aren't the host's). The `loopback` and
`tcp` backends containerize cleanly; the `verbs` backend needs host RDMA
devices (`/dev/infiniband`) and elevated privileges, so it is not a clean
container target.

**Tested & verified** (built + `docker load`ed from an x86_64-linux host):
the amd64 **minimal** image runs the server (`docker run … --help` → exit 0,
~141 MB); the amd64 **debug** image additionally carries `/bin/bash` +
coreutils (`docker exec` works); the cross **arm64** and **riscv64** images
inspect as `Architecture: arm64` / `riscv64` and the server ELF inside is
`ARM aarch64` / `UCB RISC-V` respectively. All minimal images tag
`rocm-ernic:<version>`, so a local `docker load` of one overwrites the others
— push per-arch tags or a manifest list to keep them side by side.

## Using the emulated NIC from another flake

`nixosModules.ernic-guest` adds the emulated ionic NIC to any `qemu-vm.nix`
guest, and `packages.ernic-run` starts the server around the VM launcher:

```
# guest configuration
modules = [
  "${nixpkgs}/nixos/modules/virtualisation/qemu-vm.nix"
  rocm-ernic.nixosModules.ernic-guest
  { virtualisation.ernic.enable = true; virtualisation.cores = 4; }
];

# host: start the server, export ERNIC_SOCKET, run the VM, stop the server
ernic-run --backend loopback -- ./result/bin/run-<host>-vm
```

The module sets shared memfd guest RAM, q35 with a `pcie-root-port` (`rp0`)
so QMP `device_del`/`device_add ernic0` can hot-unplug and replug the NIC, and
the JSON-form `vfio-user-pci` device. It asserts at least 4 vCPUs and a guest
kernel of 6.18 or newer. Its `ernic-rdma-up` service opens the ionic netdev
*before* loading `ionic_rdma`. Loaded first, `ionic_rdma` takes every spare
interrupt for EQs, and `ip link set up` then fails with
`qcq alloc of rx0 failed -28`.

```
nix run .#run-ionic-guest-test              # KVM if usable, else TCG
nix run .#run-ionic-guest-test -- --accel=tcg
```

This boots a guest on the loopback backend and checks `lspci 1dd8:1002`,
`rdma link` ACTIVE and `ibv_devinfo` PORT_ACTIVE, with the serial log kept in a
mktemp evidence directory. It uses the stock nixpkgs QEMU and kernel and needs
no root, `/dev/vfio` or RDMA hardware.

## Trying the emulated NIC interactively

```
nix run github:ROCm/rocm-ernic#ionic-vm
nix run .#ionic-vm -- --ssh-port 2222       # then: ssh -p 2222 root@127.0.0.1
nix run .#ionic-vm -- --accel=tcg --backend loopback
```

`ionic-vm` starts the server and boots a NixOS guest (nixpkgs
`linuxPackages_latest`, 4 vCPUs, 4 GiB) to an autologin root shell on the
serial console, with `ionic`/`ionic_rdma` bound and the RDMA port ACTIVE. The
login message lists things to try (`rdma link`, `ibv_devinfo -v`,
`ethtool -i`, `lspci -nn -d 1dd8:`). Quit with `Ctrl-a x` or `poweroff`.

The guest root is a tmpfs, so each run starts clean. The server log and socket
live in a mktemp run directory printed at start. `--ssh-port` forwards the
guest's sshd to `127.0.0.1:PORT` only and hands the host user's
`~/.ssh/*.pub` keys to the guest through the qemu-vm.nix shared directory;
root login is key-only. Like `run-ionic-guest-test`, it needs no root,
`/dev/vfio` or RDMA hardware, and uses KVM when it is usable.

## Module map

| Path | Purpose |
|---|---|
| `flake.nix` | inputs + per-system outputs |
| `nix/lib.nix` | shared build context (packages, compat, version, analysis helpers) threaded into the modules |
| `nix/libvfio-user.nix` | from-source libvfio-user (+ synthesized `.pc`) |
| `nix/packages.nix` | dependency / tool sets |
| `nix/derivation.nix` | CMake build of rocm-ernic |
| `nix/cross.nix` | cross-compile the server for a target arch (crossSystem pkgs) |
| `nix/guest-module.nix` | `nixosModules.ernic-guest`: emulated NIC for a qemu-vm.nix guest |
| `nix/ernic-run.nix` | `ernic-run`: start the server around a command, export `ERNIC_SOCKET` |
| `nix/ionic-guest-test.nix` | `run-ionic-guest-test`: guest reaches PORT_ACTIVE on the in-tree drivers |
| `nix/ionic-vm.nix` | `ionic-vm`: interactive guest with the emulated NIC, root shell, optional ssh forward |
| `nix/oci.nix` | OCI image (minimal/debug variants) via dockerTools.streamLayeredImage |
| `nix/microvms/` | per-arch run layer: TCG microvm + loopback self-test + lifecycle driver (VM arches), qemu-user smoke (armv7l/ppc64le) |
| `nix/devshell.nix`, `nix/shell-functions/` | dev shell + helpers |
| `nix/compat-cflags.nix` | gcc-15 C99-error downgrade (build + shell) |
| `nix/analysis/` | static + dynamic + fuzz analysis modules |
| `nix/analysis/triage/` | finding prioritiser (Python) |

### Launcher regression checks

`nix build .#checks.x86_64-linux.launchers --no-link -L` exercises the
packaged `ernic-run` with controlled servers and commands, including
failed startup, server death, command failures, ignored TERM, interrupted
launchers, and interactive input. It also checks guest module assertion
boundaries and JSON encoding of runtime socket paths. No VM is required.

The launcher preserves command exit codes, stops a waiting command when
its server dies, and gives owned process groups ten seconds to stop before
using KILL. Caller-owned paths are preserved on rejected startup; temporary
socket directories created by the launcher are removed when empty.
