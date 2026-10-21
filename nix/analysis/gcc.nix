# nix/analysis/gcc.nix
#
# GCC-based analysis as two full CMake builds:
#   gcc-warnings  extra -W… flags, capture the diagnostics
#   gcc-analyzer  -fanalyzer path-sensitive analysis
#
# Extra flags are injected via CMAKE_C_FLAGS.
{ ctx, src }:

let
  inherit (ctx) lib mkAnalysisDrv configureCmake;

  gccWarningFlags = [
    "-Wall" "-Wextra" "-Wpedantic"
    "-Wformat=2" "-Wformat-security"
    "-Wshadow" "-Wcast-qual" "-Wcast-align" "-Wwrite-strings"
    "-Wpointer-arith" "-Wconversion" "-Wsign-conversion"
    "-Wduplicated-cond" "-Wduplicated-branches" "-Wlogical-op"
    "-Wnull-dereference" "-Wdouble-promotion" "-Wfloat-equal"
    "-Walloca" "-Wvla"
    # C-specific
    "-Wstrict-prototypes" "-Wold-style-definition"
    "-Wmissing-prototypes" "-Wbad-function-cast"
  ];

  mkGccAnalysisBuild = name: extraFlags:
    mkAnalysisDrv {
      inherit name src;

      buildPhase = ''
        runHook preBuild
        srcTop="$PWD"
        ${configureCmake ''-DCMAKE_C_FLAGS="${lib.concatStringsSep " " extraFlags}"''}
        cmake --build "$srcTop/build" 2>&1 \
          | tee "$NIX_BUILD_TOP/build-output.log" || true
        runHook postBuild
      '';

      installPhase = ''
        mkdir -p $out
        # Strip the sandbox source prefix so report paths are repo-relative
        # (src/…), then keep only in-tree diagnostics: warnings from glib /
        # system headers are noise triage would discard anyway. The full
        # log is kept alongside.
        # -Wleading-whitespace is a gcc-15 formatting diagnostic that fires
        # on nearly every indented line of the QEMU-derived code; it is pure
        # noise for bug-finding, so drop it from the report.
        grep -E ': warning:|: error:' "$NIX_BUILD_TOP/build-output.log" \
          | sed "s|$srcTop/||g" \
          | grep -E '^src/' \
          | grep -v 'leading-whitespace' > $out/report.txt || true
        findings=$(wc -l < $out/report.txt)
        echo "$findings" > $out/count.txt
        cp "$NIX_BUILD_TOP/build-output.log" $out/full-build.log
        {
          echo "=== ${name} Analysis ==="
          echo ""
          echo "Extra flags: ${lib.concatStringsSep " " extraFlags}"
          echo "Findings: $findings warnings/errors"
        } > $out/summary.txt
      '';
    };
in
{
  gcc-warnings = mkGccAnalysisBuild "gcc-warnings" gccWarningFlags;
  gcc-analyzer = mkGccAnalysisBuild "gcc-analyzer" [
    "-fanalyzer"
    "-fdiagnostics-plain-output"
  ];
}
