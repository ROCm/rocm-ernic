# nix/packages.nix
#
# Package definitions for rocm-ernic, split (as in the xdp2 reference) into:
# - nativeBuildInputs: build-time tools that run on the build machine
# - buildInputs:       libraries needed to compile and link
# - devTools:          extra tools for the interactive dev shell only
#
# Usage:
#   packages = import ./nix/packages.nix { inherit pkgs libvfio-user; };

{ pkgs, libvfio-user }:

let
  # True when building for a different platform than the build host. The
  # cross stdenv already provides the target `cc`; adding a standalone
  # `pkgs.gcc` here would splice to the *native* build->build gcc and put a
  # plain `gcc`/`cc` on PATH that shadows the cross compiler CMake must use.
  # So the explicit gcc is native-only; under cross we rely on stdenv.cc.
  isCross = pkgs.stdenv.buildPlatform != pkgs.stdenv.hostPlatform;
in
rec {
  # Build-time tools (run on the build host).
  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.git

    # Core utilities used during build / helper scripts.
    pkgs.bash
    pkgs.coreutils
    pkgs.gnused
    pkgs.gnugrep
  ]
  # Native builds pin gcc explicitly (matching CI); cross uses stdenv.cc.
  ++ pkgs.lib.optional (!isCross) pkgs.gcc;

  # Libraries required to compile and link rocm-ernic.
  #   vfio-user   -> libvfio-user (from source, see libvfio-user.nix)
  #   glib-2.0    -> pkgs.glib (glib.dev provides pkg-config + headers)
  #   libibverbs, -> pkgs.rdma-core
  #   librdmacm
  #   json-c      -> transitive dep of libvfio-user; kept explicit for safety
  buildInputs = [
    libvfio-user
    pkgs.glib
    pkgs.rdma-core
    pkgs.json_c
  ];

  # Development-only tools (not needed to build, only for the dev workflow
  # and the analysis framework).
  devTools = [
    # Debugging / dynamic analysis
    pkgs.gdb
    pkgs.valgrind
    pkgs.strace
    pkgs.ltrace

    # Compilers / language servers
    pkgs.clang
    pkgs.clang-tools        # clang-tidy, clang-format
    pkgs.clang-analyzer     # scan-build

    # Static analysis
    pkgs.cppcheck
    pkgs.flawfinder
    pkgs.semgrep
    pkgs.include-what-you-use

    # Shell / cmake hygiene
    pkgs.shellcheck
    pkgs.cmake-format

    # Visualization / misc
    pkgs.graphviz
    pkgs.jq
  ];

  # Combined list for the dev shell (rec lets us reference the sets above
  # directly instead of re-importing this file).
  allPackages = nativeBuildInputs ++ buildInputs ++ devTools;
}
