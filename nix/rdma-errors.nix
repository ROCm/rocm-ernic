# nix/rdma-errors.nix
#
# ernic-rdma-errors (tests/guest/ernic_rdma_errors.c), packaged on its own so
# a guest can install it with its own pkgs: it needs only libibverbs, not the
# server's dependencies.  The CMake build compiles the same file, so the
# warning and analysis gates see it too.
#
# Usage:
#   ernic-rdma-errors = pkgs.callPackage ./nix/rdma-errors.nix { };

{ stdenv, lib, rdma-core }:

stdenv.mkDerivation {
  pname = "ernic-rdma-errors";
  version = "0.1.0";
  src = lib.fileset.toSource {
    root = ../tests/guest;
    fileset = ../tests/guest/ernic_rdma_errors.c;
  };
  buildInputs = [ rdma-core ];
  buildPhase = ''
    runHook preBuild
    $CC -std=gnu11 -O2 -Wall -Wextra -Werror -D_GNU_SOURCE \
      -o ernic-rdma-errors ernic_rdma_errors.c -libverbs
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    install -D -m 755 ernic-rdma-errors "$out/bin/ernic-rdma-errors"
    runHook postInstall
  '';
  meta = {
    description = "Make rocm-ernic's RDMA errors happen in a guest and check hw_counters";
    license = lib.licenses.gpl2Plus;
    mainProgram = "ernic-rdma-errors";
    platforms = lib.platforms.linux;
  };
}
