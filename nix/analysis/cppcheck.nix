# nix/analysis/cppcheck.nix
#
# cppcheck over the CMake compilation database. Using --project gives
# cppcheck the exact include paths and defines for every translation unit,
# so it analyses both our code and the QEMU-ported parsers.
{ pkgs, mkCompileDbReport }:

let
  runner = pkgs.writeShellApplication {
    name = "run-cppcheck-analysis";
    runtimeInputs = with pkgs; [ cppcheck coreutils gnugrep ];
    text = ''
      compile_db="$1"
      output_dir="$2"

      echo "=== cppcheck Analysis (C) ==="

      # --project cannot be combined with explicit source args.
      cppcheck \
        --project="$compile_db/compile_commands.json" \
        --enable=all \
        --std=c11 \
        --suppress=missingInclude \
        --suppress=missingIncludeSystem \
        --suppress=unusedFunction \
        --suppress=unmatchedSuppression \
        --xml \
        2> "$output_dir/report.xml" || true

      cppcheck \
        --project="$compile_db/compile_commands.json" \
        --enable=all \
        --std=c11 \
        --suppress=missingInclude \
        --suppress=missingIncludeSystem \
        --suppress=unusedFunction \
        --suppress=unmatchedSuppression \
        2> "$output_dir/report.txt" || true

      findings=$(grep -c '\(error\|warning\|style\|performance\|portability\)' "$output_dir/report.txt" || echo "0")
      echo "$findings" > "$output_dir/count.txt"
    '';
  };
in
mkCompileDbReport "cppcheck" runner
