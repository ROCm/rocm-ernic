# nix/lib.nix
#
# Shared build context for the rocm-ernic flake. It is built once in
# flake.nix and threaded (as `ctx`) into the build, dev shell, and analysis
# modules, so none of them has to re-import packages.nix / compat-cflags.nix
# or re-hardcode the version.
#
# Fields:
#   pkgs, lib, libvfio-user   the flake's per-system inputs, passed through
#   packages                  dependency / tool sets (nix/packages.nix)
#   compat                    gcc-15 C99-error downgrade flags (compat-cflags.nix)
#   version                   the single source of the package version
#   configureCmake            the shared `cmake -S/-B` configure line (analysis)
#   scrubStore                sed program stripping /nix/store/<hash>-<name>/
#   mkAnalysisDrv             base derivation for the CMake analysis builds
#
# Usage:
#   ctx = import ./nix/lib.nix { inherit pkgs lib libvfio-user; };

{ pkgs, lib, libvfio-user }:

let
  packages = import ./packages.nix { inherit pkgs libvfio-user; };
  compat = import ./compat-cflags.nix { };
  version = "0.2.0";

  # One cmake configure line for the analysis builds. They all configure a
  # Debug tree with Ninja and WERROR off (gcc-15 / clang warn on the
  # QEMU-ported sources); callers append tool-specific flags via `extraFlags`
  # (a single already-quoted string, may be empty). Expects `srcTop` to be
  # set to the source root in the surrounding phase.
  configureCmake = extraFlags: ''
    cmake -S "$srcTop" -B "$srcTop/build" -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DERNIC_WERROR=OFF ${extraFlags}
  '';

  # Strip /nix/store/<hash>-<name>/ prefixes so any paths in a report read as
  # repo-relative. Used by the dynamic-analysis modules.
  scrubStore = ''sed 's|/nix/store/[a-z0-9]\{32\}-[^/]*/||g' '';

  # Base derivation for the CMake-driven analysis builds: shared build inputs,
  # the gcc-15 cflag downgrade, and the "drive cmake by hand" flags. Callers
  # supply their own buildPhase / installPhase (and may add tool inputs via
  # `extraNativeBuildInputs`); anything they pass overrides the defaults here.
  mkAnalysisDrv = { name, extraNativeBuildInputs ? [ ], ... }@args:
    pkgs.stdenv.mkDerivation ({
      pname = "rocm-ernic-analysis-${name}";
      inherit version;
      nativeBuildInputs = packages.nativeBuildInputs ++ extraNativeBuildInputs;
      inherit (packages) buildInputs;
      dontUseCmakeConfigure = true;
      env.NIX_CFLAGS_COMPILE = compat.string;
      dontFixup = true;
      doCheck = false;
    } // (removeAttrs args [ "name" "extraNativeBuildInputs" ]));
in
{
  inherit pkgs lib libvfio-user packages compat version
    configureCmake scrubStore mkAnalysisDrv;
}
