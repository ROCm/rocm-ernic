#
# flake.nix for rocm-ernic
#
# A reproducible development environment, a hermetic build, and a suite of
# static, dynamic, and fuzz analysis targets for the userspace emulated RDMA
# NIC. The design is modular: this slim flake.nix wires together small
# single-purpose modules under nix/ (see nix/README.md).
#
# Enter the development shell:
#   nix develop
#
# Build the server binary:
#   nix build .#rocm-ernic      # -> ./result/bin/rocm-ernic
#
# Run the analysis (nix/README.md lists every target):
#   nix build .#analysis-deep && cat result/summary.txt
#
# If flakes are not enabled, prefix commands with:
#   nix --extra-experimental-features 'nix-command flakes' <cmd>
#
# NOTE: flakes only see git-tracked files. After adding or editing files
# under nix/ (or this flake), `git add` them before `nix build`/`nix
# develop`, or the changes are invisible to the evaluation.
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

  # Linux-only: the build and analysis depend on libvfio-user, rdma-core and
  # ptrace-based tools, none of which work on Darwin. eachSystem (not
  # eachDefaultSystem) keeps the flake from generating darwin attributes that
  # could only fail to evaluate/build.
  outputs = { self, nixpkgs, flake-utils, libvfio-user }:
    flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" ] (system:
      let
        pkgs = import nixpkgs { inherit system; };
        lib = nixpkgs.lib;

        # libvfio-user built from the pinned source input.
        libvfioUser = import ./nix/libvfio-user.nix {
          inherit lib;
          inherit (pkgs) stdenv meson ninja pkg-config json_c cmocka;
          src = libvfio-user;
        };

        # Shared build context: dependency sets, compat cflags, version, and
        # the helpers the analysis builds reuse. Built once, threaded below.
        ctx = import ./nix/lib.nix {
          inherit pkgs lib;
          libvfio-user = libvfioUser;
        };

        rocm-ernic = import ./nix/derivation.nix {
          inherit ctx;
          src = ./.;
        };

        devshell = import ./nix/devshell.nix { inherit ctx; };

        # Static, dynamic, and fuzz analysis. See nix/analysis/default.nix.
        analysis = import ./nix/analysis {
          inherit ctx;
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

          # Dynamic analysis: build + exercise on loopback.
          analysis-sanitizers = analysis.sanitizers;
          analysis-tsan = analysis.thread-sanitizer;
          analysis-valgrind = analysis.valgrind;

          # Fuzzing.
          fuzz = analysis.fuzzers;       # build harnesses + corpora
          fuzz-run = analysis.fuzz-run;  # bounded run, collect crashes
        };

        devShells.default = devshell;
      });
}
