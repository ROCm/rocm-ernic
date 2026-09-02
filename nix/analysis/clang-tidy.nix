# nix/analysis/clang-tidy.nix
#
# clang-tidy over the CMake compilation database. Runs on every .c under
# src/ (our code and the QEMU-ported parsers); clang-tidy's own check set
# is independent of the compile command's -w, so the ported sources are
# still analysed. header-filter restricts diagnostics to src/ headers.
{ pkgs, mkCompileDbReport }:

let
  runner = pkgs.writeShellApplication {
    name = "run-clang-tidy-analysis";
    runtimeInputs = with pkgs; [ clang-tools coreutils findutils gnugrep ];
    text = ''
      compile_db="$1"
      output_dir="$2"

      echo "=== clang-tidy Analysis (C) ==="

      find "$compile_db/src" -name '*.c' -print0 | \
        xargs -0 -P "$(nproc)" -I{} \
          clang-tidy \
            -p "$compile_db" \
            --header-filter='.*/src/.*' \
            --checks='-*,bugprone-*,cert-*,clang-analyzer-*,misc-*,readability-*' \
            {} \
        > "$output_dir/report.txt" 2>&1 || true

      findings=$(grep -c ': warning:\|: error:' "$output_dir/report.txt" || echo "0")
      echo "$findings" > "$output_dir/count.txt"
    '';
  };
in
mkCompileDbReport "clang-tidy" runner
