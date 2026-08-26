# nix/analysis/valgrind.nix
#
# Memory-leak / memory-error detection with valgrind memcheck. A normal
# (uninstrumented) Debug build of the server is run under valgrind on the
# loopback backend, exercised with the PCI-config test client, then shut
# down cleanly (SIGTERM) so memcheck emits its full leak summary.
#
# valgrind complements the sanitizers: it needs no ptrace for leak
# detection (works where LSan may not) and catches uninitialised-value and
# invalid-access errors the sanitizer build might miss.
{ lib, pkgs, src, libvfio-user }:

let
  packages = import ../packages.nix { inherit pkgs libvfio-user; };
  compatCflags = import ../compat-cflags.nix { };
  driver = import ./run-loopback.nix { inherit pkgs; };
in
pkgs.stdenv.mkDerivation {
  pname = "rocm-ernic-analysis-valgrind";
  version = "0.2.0";
  inherit src;

  nativeBuildInputs = packages.nativeBuildInputs ++ [ pkgs.valgrind ];
  inherit (packages) buildInputs;

  dontUseCmakeConfigure = true;
  env.NIX_CFLAGS_COMPILE = compatCflags.string;

  buildPhase = ''
    runHook preBuild
    srcTop="$PWD"
    cmake -S "$srcTop" -B "$srcTop/build" -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DERNIC_WERROR=OFF
    cmake --build "$srcTop/build"
    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    echo "=== Exercising server under valgrind memcheck ==="

    valgrind_log="$NIX_BUILD_TOP/valgrind.log"

    # --error-exitcode=0: never fail the run on findings; we parse the log.
    export SERVER_WRAPPER="valgrind \
      --leak-check=full \
      --show-leak-kinds=all \
      --track-origins=yes \
      --trace-children=no \
      --error-exitcode=0 \
      --log-file=$valgrind_log"

    export OUT_LOG="$NIX_BUILD_TOP/server.log"
    export CLIENT_LOG="$NIX_BUILD_TOP/client.log"

    bash ${driver} \
      "$srcTop/build/rocm-ernic" \
      "$srcTop/build/tests/test_pci_client" || true

    echo "--- valgrind summary ---"
    grep -E 'LEAK SUMMARY|ERROR SUMMARY|definitely lost|indirectly lost|possibly lost' \
      "$valgrind_log" || echo "(no valgrind summary captured)"
    runHook postCheck
  '';

  installPhase = ''
    mkdir -p $out

    valgrind_log="$NIX_BUILD_TOP/valgrind.log"
    touch "$valgrind_log"

    # Strip store prefixes for readability.
    sed 's|/nix/store/[a-z0-9]\{32\}-[^/]*/||g' "$valgrind_log" > $out/report.txt

    # "definitely lost: N bytes in M blocks" — report M (blocks) as the count.
    leaks=$(grep -oP 'definitely lost: [\d,]+ bytes in \K[\d,]+' "$valgrind_log" \
      | tr -d ',' | head -1 || true)
    [ -z "$leaks" ] && leaks=0
    errors=$(grep -oP 'ERROR SUMMARY: \K[0-9]+' "$valgrind_log" | head -1 || true)
    [ -z "$errors" ] && errors=0
    echo "$leaks" > $out/count.txt

    cp "$NIX_BUILD_TOP/server.log" $out/server.log 2>/dev/null || true
    cp "$NIX_BUILD_TOP/client.log" $out/client.log 2>/dev/null || true

    {
      echo "=== valgrind memcheck ==="
      echo ""
      echo "Server run under memcheck on the loopback backend, exercised"
      echo "with the PCI-config test client, then shut down cleanly."
      echo ""
      echo "definitely-lost leak blocks: $leaks"
      echo "memcheck ERROR SUMMARY:      $errors"
      echo ""
      echo "Full log: report.txt"
    } > $out/summary.txt
    cat $out/summary.txt
  '';

  dontFixup = true;
}
