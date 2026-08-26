# nix/analysis/default.nix
#
# Static-analysis entry point for rocm-ernic. Ported from the xdp2
# reference framework, re-implemented for this CMake project.
#
# Tools (userspace src/ only):
#   clang-tidy, cppcheck           via the CMake compilation database
#   flawfinder, semgrep            raw-source scanners
#   gcc-warnings, gcc-analyzer     full builds with extra diagnostics
#   clang-analyzer                 scan-build path-sensitive analysis
#
# Aggregate levels (each also runs the triage prioritiser):
#   quick     = clang-tidy + cppcheck
#   standard  = + flawfinder + clang-analyzer + gcc-warnings
#   deep      = + gcc-analyzer + semgrep
#
# Usage:
#   nix build .#analysis-quick | .#analysis-standard | .#analysis-deep
#   nix build .#analysis-cppcheck   (etc. per-tool)

{ pkgs, lib, libvfio-user, src }:

let
  python = pkgs.python3;

  # ── Compilation database ────────────────────────────────────────
  compileDb = import ./compile-db.nix { inherit pkgs lib libvfio-user; };

  # ── Helpers ─────────────────────────────────────────────────────
  # Tools that consume the compilation DB: script args = (compileDb, out).
  mkCompileDbReport = name: script:
    pkgs.runCommand "rocm-ernic-analysis-${name}"
      { nativeBuildInputs = [ script ]; }
      ''
        mkdir -p $out
        ${lib.getExe script} ${compileDb} $out
      '';

  # Tools that scan raw source: script args = (src, out).
  mkSourceReport = name: script:
    pkgs.runCommand "rocm-ernic-analysis-${name}"
      { nativeBuildInputs = [ script ]; }
      ''
        mkdir -p $out
        ${lib.getExe script} ${src} $out
      '';

  # ── Individual tool targets ─────────────────────────────────────
  clang-tidy = import ./clang-tidy.nix { inherit pkgs mkCompileDbReport; };
  cppcheck = import ./cppcheck.nix { inherit pkgs mkCompileDbReport; };
  flawfinder = import ./flawfinder.nix { inherit pkgs mkSourceReport; };
  semgrep = import ./semgrep.nix { inherit pkgs mkSourceReport; };
  gccTargets = import ./gcc.nix { inherit lib pkgs src libvfio-user; };
  clang-analyzer = import ./clang-analyzer.nix { inherit lib pkgs src libvfio-user; };

  # ── Dynamic analysis (Phase C) ──────────────────────────────────
  # These build + run the server on the loopback backend; they are not
  # part of the static triage aggregates.
  sanitizerTargets = import ./sanitizers.nix { inherit lib pkgs src libvfio-user; };
  valgrind = import ./valgrind.nix { inherit lib pkgs src libvfio-user; };

  # ── Triage ──────────────────────────────────────────────────────
  triagePath = ./triage;

  # Build an aggregate: symlink each tool's output under $out/<name>, run
  # the triage prioritiser, and print a summary table.
  mkAggregate = level: tools: summaryExtra:
    pkgs.runCommand "rocm-ernic-analysis-${level}"
      { nativeBuildInputs = [ python ]; }
      ''
        mkdir -p $out
        ${lib.concatMapStringsSep "\n"
            (t: "ln -s ${t.drv} $out/${t.name}") tools}

        python3 ${triagePath} $out --output-dir $out/triage

        {
          echo "=== Analysis Summary (${level}) ==="
          echo ""
          ${lib.concatMapStringsSep "\n"
              (t: ''echo "${lib.fixedWidthString 16 " " (t.name + ":")} $(cat ${t.drv}/count.txt) findings"'')
              tools}
          echo "${lib.fixedWidthString 16 " " "triage:"} $(cat $out/triage/count.txt 2>/dev/null || echo 0) high-confidence findings"
          echo ""
          ${summaryExtra}
        } > $out/summary.txt
        cat $out/summary.txt
      '';

  quick = mkAggregate "quick"
    [ { name = "clang-tidy"; drv = clang-tidy; }
      { name = "cppcheck"; drv = cppcheck; }
    ]
    ''echo "Run 'nix build .#analysis-standard' for a broader sweep."'';

  standard = mkAggregate "standard"
    [ { name = "clang-tidy"; drv = clang-tidy; }
      { name = "cppcheck"; drv = cppcheck; }
      { name = "flawfinder"; drv = flawfinder; }
      { name = "clang-analyzer"; drv = clang-analyzer; }
      { name = "gcc-warnings"; drv = gccTargets.gcc-warnings; }
    ]
    ''echo "Run 'nix build .#analysis-deep' for gcc -fanalyzer + semgrep."'';

  deep = mkAggregate "deep"
    [ { name = "clang-tidy"; drv = clang-tidy; }
      { name = "cppcheck"; drv = cppcheck; }
      { name = "flawfinder"; drv = flawfinder; }
      { name = "clang-analyzer"; drv = clang-analyzer; }
      { name = "gcc-warnings"; drv = gccTargets.gcc-warnings; }
      { name = "gcc-analyzer"; drv = gccTargets.gcc-analyzer; }
      { name = "semgrep"; drv = semgrep; }
    ]
    ''echo "All analysis tools completed."'';
in
{
  inherit
    compileDb
    clang-tidy cppcheck flawfinder semgrep clang-analyzer
    quick standard deep
    valgrind;
  inherit (gccTargets) gcc-warnings gcc-analyzer;
  inherit (sanitizerTargets) sanitizers thread-sanitizer;
}
