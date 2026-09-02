# nix/cross.nix
#
# Cross-compile rocm-ernic for a non-native target architecture.
#
# rocm-ernic is a plain CMake + Ninja project with no host code-generator,
# so — unlike the xdp2 reference — it needs no host/target compiler split:
# the cross stdenv drives everything. We just build a `crossSystem` pkgs
# set, rebuild the from-source libvfio-user and the shared `ctx` from it,
# and reuse nix/derivation.nix unchanged. nixpkgs' input splicing routes
# packages.nativeBuildInputs to build-host tools and packages.buildInputs
# to target libraries automatically (nix/packages.nix also drops the
# explicit gcc under cross so nothing shadows the cross `cc`).
#
# We import nixpkgs with an explicit crossSystem (rather than reading
# pkgs.pkgsCross.*) so a `crossFixes` overlay can attach — used to disable
# test phases for any dependency whose tests fail under cross. It starts
# empty; the spike adds entries only where a build actually needs one.
#
# Usage (from flake.nix, gated on the build host being x86_64-linux):
#   (import ./nix/cross.nix {
#      inherit nixpkgs lib;
#      buildSystem = "x86_64-linux";
#      targetArch = "aarch64";
#      libvfioUserSrc = libvfio-user;   # the flake's pinned source input
#   }).rocm-ernic

{ nixpkgs
, lib
, buildSystem
, targetArch
, libvfioUserSrc
}:

let
  # Map our short arch names onto nixpkgs' vetted crossSystem descriptors.
  # Using lib.systems.examples.<attr> (rather than a bare "arm/ppc" system
  # string) pins the ABI/endianness explicitly:
  #   armv7l  -> hard-float EABI      ppc64le -> 64-bit little-endian ELFv2
  examples = lib.systems.examples;
  crossSystems = {
    aarch64 = examples.aarch64-multiplatform;
    riscv64 = examples.riscv64;
    armv7l  = examples.armv7l-hf-multiplatform;
    ppc64le = examples.powernv;
  };
  crossSystem = crossSystems.${targetArch}
    or (throw "cross.nix: unsupported targetArch '${targetArch}' "
      + "(expected one of ${lib.concatStringsSep ", " (lib.attrNames crossSystems)})");

  # doCheck=false overrides for dependencies whose test suites fail under
  # cross/emulation. Kept empty until a spike build proves one is needed —
  # our closure (glib, json_c, rdma-core, from-source libvfio-user) is far
  # smaller than xdp2's, so it may stay empty. Add entries like:
  #   somePkg = prev.somePkg.overrideAttrs (_: { doCheck = false; });
  crossFixes = final: prev: {
  };

  crossPkgs = import nixpkgs {
    localSystem = buildSystem;
    inherit crossSystem;
    config = { allowUnfree = true; };
    overlays = [ crossFixes ];
  };

  # Rebuild the from-source libvfio-user with the cross toolchain. The
  # module already splits meson/ninja/pkg-config (native) from json_c/cmocka
  # (target) correctly and sets doCheck=false, so it cross-compiles as-is.
  libvfioUser = import ./libvfio-user.nix {
    inherit lib;
    inherit (crossPkgs) stdenv meson ninja pkg-config json_c cmocka;
    src = libvfioUserSrc;
  };

  # The shared build context, rebuilt from the cross pkgs set.
  ctx = import ./lib.nix {
    pkgs = crossPkgs;
    inherit lib;
    libvfio-user = libvfioUser;
  };

  rocm-ernic = import ./derivation.nix {
    inherit ctx;
    src = ./..;
    # Also install test_pci_client: the microvm run layer exercises the
    # server with it on the loopback backend inside the guest.
    installTests = true;
  };
in
{
  inherit crossPkgs libvfioUser ctx rocm-ernic;
}
