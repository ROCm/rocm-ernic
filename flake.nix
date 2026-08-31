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
# Cross-compile for another architecture (from an x86_64-linux host), and
# exercise the result under QEMU emulation:
#   nix build .#rocm-ernic-aarch64     # also: -riscv64, -armv7l, -ppc64le
#   nix run   .#run-aarch64-tests      # aarch64/riscv64: boot a microvm + self-test
#   nix run   .#run-armv7l-tests       # armv7l/ppc64le: qemu-user smoke (see nix/microvms)
#
# Build an OCI container image (minimal or debug; amd64 or cross arm64/riscv64):
#   nix build .#oci-image && ./result | docker load   # also: -debug, -aarch64, -riscv64
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

    # microvm.nix powers the emulated-run layer (nix/microvms/): booting the
    # cross-built server under QEMU/TCG to exercise it on non-x86 arches.
    microvm = {
      url = "github:astro/microvm.nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  # Linux-only: the build and analysis depend on libvfio-user, rdma-core and
  # ptrace-based tools, none of which work on Darwin. eachSystem (not
  # eachDefaultSystem) keeps the flake from generating darwin attributes that
  # could only fail to evaluate/build.
  outputs = { self, nixpkgs, flake-utils, libvfio-user, microvm }:
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

        # Cross-compiled server binaries. Only meaningful when the build
        # host is x86_64-linux (the cross toolchains target from there);
        # on a native aarch64 host the default package already covers arm.
        # See nix/cross.nix. Each arch is additive: rocm-ernic-<arch>.
        # crossResults holds the full per-arch cross.nix output (crossPkgs +
        # the cross rocm-ernic), computed once and reused by the OCI images.
        crossArches = [ "aarch64" "riscv64" "armv7l" "ppc64le" ];
        crossResults =
          if system == "x86_64-linux" then
            lib.genAttrs crossArches (arch: import ./nix/cross.nix {
              inherit nixpkgs lib;
              buildSystem = system;
              targetArch = arch;
              libvfioUserSrc = libvfio-user;
            })
          else { };
        crossPackages = lib.mapAttrs'
          (arch: res: lib.nameValuePair "rocm-ernic-${arch}" res.rocm-ernic)
          crossResults;

        # OCI images (nix/oci.nix), built straight from the closure via
        # dockerTools.streamLayeredImage. A minimal (distroless) and a debug
        # (adds a shell) variant, for amd64 (the native host) and — from an
        # x86_64 host — cross arm64/riscv64 images reusing crossResults.
        # loopback/tcp backends containerize cleanly; verbs needs host RDMA
        # devices. Load: nix build .#oci-image && ./result | docker load
        mkOci = args: import ./nix/oci.nix ({ inherit pkgs lib rocm-ernic; } // args);
        # Cross arches we ship container images for (subset of crossArches;
        # armv7l/ppc64le are build-only here, no image).
        ociCrossArches = [ "aarch64" "riscv64" ];
        mkCrossOci = arch:
          let r = crossResults.${arch}; in [
            { name = "oci-image-${arch}";
              value = mkOci { pkgs = r.crossPkgs; rocm-ernic = r.rocm-ernic; variant = "minimal"; }; }
            { name = "oci-image-${arch}-debug";
              value = mkOci { pkgs = r.crossPkgs; rocm-ernic = r.rocm-ernic; variant = "debug"; }; }
          ];
        ociPackages =
          {
            oci-image       = mkOci { variant = "minimal"; };
            oci-image-debug = mkOci { variant = "debug"; };
          }
          // lib.optionalAttrs (system == "x86_64-linux")
               (lib.listToAttrs (lib.concatMap mkCrossOci ociCrossArches));

        # Emulated-run layer: per-arch exercise of the cross-built server.
        # aarch64/riscv64 (+ an x86_64/KVM baseline) boot a full NixOS microvm
        # under QEMU/TCG and run the loopback self-test in-guest; armv7l/ppc64le
        # fall back to a qemu-user smoke (an upstream microvm.nix binary-name
        # bug blocks their VM boot). Exposes run-<arch>-tests for every arch,
        # plus microvm-<arch> for the VM arches. See nix/microvms.
        microvms =
          if system == "x86_64-linux" then
            import ./nix/microvms {
              inherit pkgs lib microvm nixpkgs;
              buildSystem = system;
              libvfioUserSrc = libvfio-user;
            }
          else null;
        vmPackages = if microvms != null then microvms.packages else { };
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
        }
        # Cross-compiled server binaries (rocm-ernic-<arch>) and the
        # emulated-run VMs (microvm-<arch>, run-<arch>-tests), added only on
        # an x86_64-linux build host. See nix/cross.nix and nix/microvms.
        // crossPackages
        // vmPackages
        # OCI images: oci-image[-debug] (amd64) always, plus
        # oci-image-{aarch64,riscv64}[-debug] on an x86_64 host. See nix/oci.nix.
        // ociPackages;

        devShells.default = devshell;
      });
}
