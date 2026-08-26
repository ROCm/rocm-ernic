#
# flake.nix for rocm-ernic
#
# Provides a reproducible development environment and build for the
# userspace emulated RDMA NIC. The design follows the modular xdp2 flake:
# a slim flake.nix wiring together small single-purpose modules under
# nix/.
#
# Enter the development shell:
#   nix develop
#
# Build the server binary:
#   nix build .#rocm-ernic      # -> ./result/bin/rocm-ernic
#
# If flakes are not enabled, prefix commands with:
#   nix --extra-experimental-features 'nix-command flakes' <cmd>
#
# NOTE: flakes only see git-tracked files. After adding or editing files
# under nix/ (or this flake), `git add` them before `nix build`/`nix
# develop`, or the changes are invisible to the evaluation.
#
# Static-analysis, sanitizer, memory-leak and fuzzing targets are added
# in later phases; run `nix flake show` to list what is available.
#
{
  description = "rocm-ernic — userspace emulated RDMA NIC for virtual machines";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";

    # libvfio-user is not in nixpkgs; pin the source and build it from
    # nix/libvfio-user.nix. flake = false: it has no flake of its own.
    libvfio-user = {
      url = "github:nutanix/libvfio-user";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, libvfio-user }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        lib = nixpkgs.lib;

        # libvfio-user built from the pinned source input.
        libvfioUser = import ./nix/libvfio-user.nix {
          inherit lib;
          inherit (pkgs) stdenv meson ninja pkg-config json_c cmocka;
          src = libvfio-user;
        };

        packages = import ./nix/packages.nix {
          inherit pkgs;
          libvfio-user = libvfioUser;
        };

        rocm-ernic = import ./nix/derivation.nix {
          inherit pkgs lib;
          libvfio-user = libvfioUser;
          src = ./.;
        };

        devshell = import ./nix/devshell.nix {
          inherit pkgs packages;
        };

        # Static-analysis framework (Phase B). See nix/analysis/default.nix.
        analysis = import ./nix/analysis {
          inherit pkgs lib;
          libvfio-user = libvfioUser;
          src = ./.;
        };
      in
      {
        packages = {
          inherit rocm-ernic;
          libvfio-user = libvfioUser;
          default = rocm-ernic;

          # Compilation database (consumed by clang-tidy / cppcheck).
          compile-db = analysis.compileDb;

          # Aggregate analysis levels.
          analysis-quick = analysis.quick;
          analysis-standard = analysis.standard;
          analysis-deep = analysis.deep;

          # Per-tool analysis targets.
          analysis-clang-tidy = analysis.clang-tidy;
          analysis-cppcheck = analysis.cppcheck;
          analysis-flawfinder = analysis.flawfinder;
          analysis-semgrep = analysis.semgrep;
          analysis-clang-analyzer = analysis.clang-analyzer;
          analysis-gcc-warnings = analysis.gcc-warnings;
          analysis-gcc-analyzer = analysis.gcc-analyzer;
        };

        devShells.default = devshell;
      });
}
