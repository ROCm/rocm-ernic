# nix/analysis/gcc.nix
#
# GCC-based analysis as two full CMake builds:
#   gcc-warnings  extra -W… flags, capture the diagnostics
#   gcc-analyzer  -fanalyzer path-sensitive analysis
#
# Extra flags are injected via CMAKE_C_FLAGS. Note the repo applies `-w`
# per-file to the QEMU-ported sources (CMakeLists.txt:229), so these extra
# warnings land on our own src/rocm_ernic_*.c — cppcheck / clang-tidy /
# flawfinder / semgrep cover the ported parsers instead.
{ lib, pkgs, src, libvfio-user }:

let
  packages = import ../packages.nix { inherit pkgs libvfio-user; };
  compatCflags = import ../compat-cflags.nix { };

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
    pkgs.stdenv.mkDerivation {
      pname = "rocm-ernic-analysis-${name}";
      version = "0.2.0";
      inherit src;

      inherit (packages) nativeBuildInputs buildInputs;

      dontUseCmakeConfigure = true;
      env.NIX_CFLAGS_COMPILE = compatCflags.string;

      buildPhase = ''
        runHook preBuild
        srcTop="$PWD"
        cmake -S "$srcTop" -B "$srcTop/build" -G Ninja \
          -DCMAKE_BUILD_TYPE=Debug \
          -DERNIC_WERROR=OFF \
          -DCMAKE_C_FLAGS="${lib.concatStringsSep " " extraFlags}"
        cmake --build "$srcTop/build" 2>&1 \
          | tee "$NIX_BUILD_TOP/build-output.log" || true
        runHook postBuild
      '';

      installPhase = ''
        mkdir -p $out
        # Strip the sandbox source prefix so report paths are repo-relative
        # (src/…), then keep only in-tree diagnostics: warnings from glib /
        # system headers are noise triage would discard anyway, and the
        # QEMU-ported sources are built -w. The full log is kept alongside.
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

      dontFixup = true;
      doCheck = false;
    };
in
{
  gcc-warnings = mkGccAnalysisBuild "gcc-warnings" gccWarningFlags;
  gcc-analyzer = mkGccAnalysisBuild "gcc-analyzer" [
    "-fanalyzer"
    "-fdiagnostics-plain-output"
  ];
}
