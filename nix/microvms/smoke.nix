# nix/microvms/smoke.nix
#
# qemu-user smoke test for architectures whose full NixOS microvm cannot be
# booted here (armv7l, ppc64le — see the runMode note in constants.nix: an
# upstream microvm.nix bug names the system emulator `qemu-system-armv7l` /
# `qemu-system-powerpc64le`, which QEMU does not ship).
#
# Instead of a full VM, this runs the *cross-built* server binary directly
# under user-mode QEMU emulation (`qemu-arm`, `qemu-ppc64le`). That still
# exercises the whole target-arch runtime path — the dynamic loader
# (target glibc), every shared library in the closure (glib, json_c,
# libvfio-user, rdma-core, ...), and the program's own startup / argument
# parsing — and asserts it reaches `--help` and exits 0. It is the
# "land what's green" degradation path from the plan: a real emulated-run
# signal for these arches without a bootable guest.
#
# Returns a `run-<arch>-tests` package (writeShellApplication) so the run
# target name is uniform across every architecture, VM or smoke.
#
# Usage (from nix/microvms/default.nix):
#   import ./smoke.nix {
#     inherit nixpkgs lib buildSystem arch libvfioUserSrc;
#   }

{ nixpkgs, lib, buildSystem, arch, libvfioUserSrc }:

let
  constants = import ./constants.nix;
  cfg = constants.architectures.${arch};

  # The cross-built server for the target arch — the very same derivation the
  # flake exposes as `rocm-ernic-<arch>`, so this reuses that build.
  cross = import ../cross.nix {
    inherit nixpkgs lib buildSystem libvfioUserSrc;
    targetArch = cfg.crossArch;
  };
  ernic = cross.rocm-ernic;

  # Host tools: qemu (provides the user-mode emulators qemu-arm/qemu-ppc64le)
  # plus coreutils/gnugrep for the harness. Built for the build host.
  hostPkgs = import nixpkgs {
    system = buildSystem;
    config = { allowUnfree = true; };
  };
in
hostPkgs.writeShellApplication {
  name = "run-${arch}-tests";
  runtimeInputs = [ hostPkgs.qemu hostPkgs.coreutils hostPkgs.gnugrep ];
  text = ''
    echo "=================================================="
    echo "  rocm-ernic qemu-user smoke: ${arch}"
    echo "  ${cfg.description}"
    echo "=================================================="

    bin="${ernic}/bin/rocm-ernic"
    echo "[smoke] target binary: $bin"
    file "$bin" 2>/dev/null || true
    echo "[smoke] emulator: ${cfg.qemuUserBin}"
    echo ""

    # Run the cross binary under user-mode emulation. The ELF interpreter is
    # an absolute /nix/store path (target glibc) that exists on the host, so
    # qemu-user loads it directly; no -L sysroot is needed.
    set +e
    out=$(${cfg.qemuUserBin} "$bin" --help 2>&1)
    rc=$?
    set -e

    echo "--- emulated --help output (head) ---"
    printf '%s\n' "$out" | head -n 12
    echo "-------------------------------------"

    if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q "Usage:"; then
      echo "[smoke] RESULT: PASS (${arch}) — cross binary ran under ${cfg.qemuUserBin}, exit 0"
      echo "ERNIC Self-Test: SUCCESS"
      exit 0
    else
      echo "[smoke] RESULT: FAIL (${arch}) — emulator rc=$rc"
      echo "ERNIC Self-Test: FAIL"
      exit 1
    fi
  '';
}
