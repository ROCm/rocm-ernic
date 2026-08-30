# nix/analysis/clang-analyzer.nix
#
# Clang Static Analyzer (scan-build) over the CMake build. scan-build wraps
# both the configure and build steps so cmake picks up its ccc-analyzer as
# the compiler ($CC) and every translation unit is analysed. C-specific
# checkers only.
{ ctx, src }:

let
  inherit (ctx) pkgs lib mkAnalysisDrv;

  scanBuildCheckers = lib.concatStringsSep " " [
    "-enable-checker core.NullDereference"
    "-enable-checker core.DivideZero"
    "-enable-checker core.UndefinedBinaryOperatorResult"
    "-enable-checker core.uninitialized.Assign"
    "-enable-checker security.insecureAPI.getpw"
    "-enable-checker security.insecureAPI.gets"
    "-enable-checker security.insecureAPI.vfork"
    "-enable-checker unix.Malloc"
    "-enable-checker unix.MallocSizeof"
    "-enable-checker unix.MismatchedDeallocator"
    "-enable-checker alpha.security.ArrayBoundV2"
    "-enable-checker alpha.unix.SimpleStream"
  ];

  # Configure + build in one script so both run under scan-build's
  # compiler interception. $1 = source root. (This keeps its own cmake line
  # rather than the shared ctx.configureCmake: scan-build invokes it with a
  # positional source-root arg, not the $srcTop the other builds use.)
  analyzeScript = pkgs.writeShellScript "analyze-build" ''
    set -e
    cmake -S "$1" -B "$1/build" -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug -DERNIC_WERROR=OFF
    cmake --build "$1/build"
  '';
in
mkAnalysisDrv {
  name = "clang-analyzer";
  inherit src;

  extraNativeBuildInputs = [ pkgs.clang pkgs.clang-analyzer ];

  buildPhase = ''
    runHook preBuild
    srcTop="$PWD"

    # Use clang-analyzer's scan-build explicitly: the copy that pkgs.clang
    # exposes has a /usr/bin/env shebang that fails in the sandbox.
    # scan-build sets CC/CXX to its analyzer wrapper for the wrapped
    # command; cmake bakes that in as the compiler, so ninja's compiles run
    # under the analyzer. Configure + build both run inside the wrapper.
    ${pkgs.clang-analyzer}/bin/scan-build \
      --use-analyzer=${pkgs.clang}/bin/clang \
      ${scanBuildCheckers} \
      -o "$NIX_BUILD_TOP/scan-results" \
      bash ${analyzeScript} "$srcTop" \
      2>&1 | tee "$NIX_BUILD_TOP/scan-build.log" || true

    runHook postBuild
  '';

  installPhase = ''
    mkdir -p $out

    if [ -d "$NIX_BUILD_TOP/scan-results" ] && \
       [ "$(ls -A "$NIX_BUILD_TOP/scan-results" 2>/dev/null)" ]; then
      mkdir -p $out/html-report
      cp -r "$NIX_BUILD_TOP/scan-results"/* $out/html-report/ 2>/dev/null || true
    fi

    # scan-build prints "scan-build: N bugs found."
    findings=$(grep -oP '\d+ bugs? found' "$NIX_BUILD_TOP/scan-build.log" \
      | grep -oP '^\d+' | head -1 || echo "0")
    [ -z "$findings" ] && findings=0
    echo "$findings" > $out/count.txt

    # Repo-relative diagnostic lines for triage.
    grep -E ': warning:|: error:' "$NIX_BUILD_TOP/scan-build.log" \
      | sed "s|$srcTop/||g" > $out/report.txt || true

    {
      echo "=== Clang Static Analyzer (C) ==="
      echo ""
      echo "Path-sensitive analysis with C-specific checkers."
      echo "Findings: $findings"
    } > $out/summary.txt
  '';
}
