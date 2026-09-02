# nix/microvms/default.nix
#
# Aggregator for the rocm-ernic microvm run layer. Per architecture it builds
# one of two run strategies, selected by `runMode` in constants.nix:
#
#   runMode = "vm"     a NixOS microvm (nix/microvms/mkVm.nix) booted under
#                      QEMU + a host-side lifecycle driver (nix/microvms/lib.nix)
#                        -> microvm-<arch>    (./result/bin/microvm-run boots it)
#                        -> run-<arch>-tests  (boot, watch for the verdict, tear down)
#
#   runMode = "smoke"  a qemu-user smoke test (nix/microvms/smoke.nix) for
#                      arches whose full VM cannot boot here (armv7l, ppc64le)
#                        -> run-<arch>-tests  (run the cross binary under
#                                              user-mode QEMU, assert exit 0)
#
# The run-<arch>-tests name is therefore uniform across every arch; only the
# VM arches additionally expose microvm-<arch>.
#
# Only meaningful on an x86_64-linux build host (the cross toolchains + TCG
# emulation target from there); flake.nix gates the import accordingly.
#
# Usage (from flake.nix):
#   microvms = import ./nix/microvms {
#     inherit pkgs lib microvm nixpkgs;
#     buildSystem = system;
#     libvfioUserSrc = libvfio-user;
#   };
#   packages = { ... } // microvms.packages;

{ pkgs, lib, microvm, nixpkgs, buildSystem, libvfioUserSrc }:

let
  constants = import ./constants.nix;
  vmlib = import ./lib.nix { inherit pkgs lib; };

  # x86_64 is a KVM baseline for shaking out the harness; aarch64/riscv64 boot
  # full TCG microvms; armv7l/ppc64le fall back to a qemu-user smoke test
  # (runMode in constants.nix).
  supportedArchs = [ "x86_64" "aarch64" "riscv64" "armv7l" "ppc64le" ];

  isVm = arch: constants.architectures.${arch}.runMode == "vm";
  vmArchs = lib.filter isVm supportedArchs;
  smokeArchs = lib.filter (arch: !(isVm arch)) supportedArchs;

  # Full-VM arches: a microvm runner + a lifecycle driver.
  vms = lib.genAttrs vmArchs (arch:
    import ./mkVm.nix {
      inherit pkgs lib microvm nixpkgs buildSystem arch libvfioUserSrc;
    });

  vmRunners = lib.genAttrs vmArchs (arch:
    vmlib.mkRunner {
      inherit arch;
      vm = vms.${arch};
      ports = constants.getPorts arch;
      timeouts = constants.getTimeouts arch;
      hostname = constants.getHostname arch;
      description = constants.architectures.${arch}.description;
    });

  # Smoke arches: run the cross binary under user-mode QEMU.
  smokeRunners = lib.genAttrs smokeArchs (arch:
    import ./smoke.nix {
      inherit nixpkgs lib buildSystem arch libvfioUserSrc;
    });

  runners = vmRunners // smokeRunners;

  packages =
    lib.listToAttrs (lib.concatMap
      (arch: [ { name = "microvm-${arch}"; value = vms.${arch}; } ])
      vmArchs)
    // lib.mapAttrs' (arch: runner:
        lib.nameValuePair "run-${arch}-tests" runner)
      runners;
in
{
  inherit vms runners packages supportedArchs vmArchs smokeArchs;
}
