# nix/devshell.nix
#
# Development shell for rocm-ernic.
#
# Mirrors the xdp2 reference structure (a mkShell whose shellHook splices
# in bash functions from nix/shell-functions/*.nix) but stripped down to
# what a CMake project needs — no libclang/cppfront/locale/nix-patch
# machinery.
#
# Usage:
#   devshell = import ./nix/devshell.nix { inherit pkgs packages; };
#   devShells.default = devshell;

{ pkgs, packages }:

let
  buildFns = import ./shell-functions/build.nix { };
  cleanFns = import ./shell-functions/clean.nix { };
  helpFns = import ./shell-functions/help.nix { };
  compatCflags = import ./compat-cflags.nix { };

  colored-prompt = ''
    export PS1="\[\033[0;32m\][rocm-ernic] \[\033[01;34m\][\u@\h:\w]\$ \[\033[0m\]"
  '';

  welcome = ''
    echo "=== rocm-ernic dev shell ==="
    echo "  cmake $(cmake --version | head -1 | awk '{print $3}'), $(gcc --version | head -1)"
    echo "  Debugging: gdb, valgrind, strace, ltrace"
    echo "  Analysis:  cppcheck, clang-tidy, flawfinder, semgrep, scan-build"
    echo "  Type 'ernic-help' for commands."
  '';
in
pkgs.mkShell {
  packages = packages.allPackages;

  # Same gcc-15 C99-error downgrade the Nix build uses, so an in-tree
  # `ernic-build` compiles the QEMU-ported sources too. See
  # nix/compat-cflags.nix. Appended so it does not clobber any value the
  # developer set before entering the shell.
  NIX_CFLAGS_COMPILE = compatCflags.string;

  shellHook = ''
    ${buildFns}
    ${cleanFns}
    ${helpFns}

    # Pin the in-tree build to gcc, matching the Nix package build and the
    # project's primary CI. This is set in the shellHook (not as a mkShell
    # env attr) because pkgs.clang's setup hook exports CC=clang during
    # shell init; the hook runs first, so we override it here. clang is
    # still on PATH for analysis and for explicit clang builds:
    #   ernic-configure -DCMAKE_C_COMPILER=clang
    export CC=gcc
    export CXX=g++

    ${colored-prompt}
    ${welcome}
  '';
}
