# nix/compat-cflags.nix
#
# Compiler-compatibility flags shared by the Nix build (derivation.nix) and
# the dev shell (devshell.nix).
#
# nixpkgs ships gcc 15, which promotes implicit-function-declaration,
# int-conversion and implicit-int from warnings to hard errors by default
# (a C99 cleanup landed in gcc 14). QEMU-derived sources have tripped
# these, and the repo's per-file `-w` on the vendored sources under
# third-party/ silences warnings but NOT these default-on errors.
# Downgrading them to warnings makes the build behave as it does on the
# project's CI (older gcc), without touching the repo's CMake or the
# vendored source.
#
# Exposed both as a Nix list and as a space-joined string for
# NIX_CFLAGS_COMPILE.
{ }:
rec {
  list = [
    "-Wno-error=implicit-function-declaration"
    "-Wno-error=int-conversion"
    "-Wno-error=implicit-int"
  ];
  string = builtins.concatStringsSep " " list;
}
