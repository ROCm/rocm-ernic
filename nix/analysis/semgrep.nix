# nix/analysis/semgrep.nix
#
# semgrep pattern-based scan with the project's C ruleset
# (semgrep-rules.yaml). Scans raw source under src/ and third-party/, so
# it covers the vendored QEMU sources as well as our own code. Runs fully
# offline.
#
# Before scanning, each rule fixture in semgrep-tests/ is run with
# `semgrep --test` (a `ruleid:` line must be reported, an `ok:` line must
# not), and a rule that fails to parse fails the build: semgrep otherwise
# skips such a rule with only an entry in the JSON "errors" list, so the
# rule would silently find nothing.
{ pkgs, mkSourceReport }:

let
  rulesFile = ./semgrep-rules.yaml;
  testsDir = ./semgrep-tests;

  runner = pkgs.writeShellApplication {
    name = "run-semgrep-analysis";
    runtimeInputs = with pkgs; [ semgrep coreutils gnugrep cacert jq ];
    text = ''
      source_dir="$1"
      output_dir="$2"

      echo "=== semgrep Analysis ==="

      export SEMGREP_ENABLE_VERSION_CHECK=0
      export SEMGREP_SEND_METRICS=off
      export SSL_CERT_FILE="${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
      export OTEL_TRACES_EXPORTER=none
      # Keep Semgrep settings/logs writable without changing the caller's HOME.
      semgrep_home="$(mktemp -d)"
      trap 'rm -rf "$semgrep_home"' EXIT
      export SEMGREP_SETTINGS_FILE="$semgrep_home/settings.yml"
      export SEMGREP_LOG_FILE="$semgrep_home/semgrep.log"

      # One fixture per run: semgrep --test pairs a single target file with
      # the config, and exits non-zero on a missed or unexpected finding.
      for fixture in ${testsDir}/*.c; do
        echo "semgrep --test $(basename "$fixture")"
        semgrep --test --config ${rulesFile} "$fixture" \
          > "$output_dir/rule-test-$(basename "$fixture" .c).txt" 2>&1 || {
          cat "$output_dir/rule-test-$(basename "$fixture" .c).txt"
          echo "semgrep rule test failed: $(basename "$fixture")" >&2
          exit 1
        }
      done

      semgrep \
        --config ${rulesFile} \
        --json \
        --metrics=off \
        --no-git-ignore \
        "$source_dir/src" \
        "$source_dir/third-party" \
        > "$output_dir/report.json" 2> "$output_dir/scan-errors.txt" || {
          cat "$output_dir/scan-errors.txt" >&2
          echo "semgrep: scan failed" >&2
          exit 1
        }

      # Validate the structure and the engine errors before counting the
      # findings. semgrep's stderr goes to scan-errors.txt so report.json is
      # only JSON, and report.txt is derived from it rather than a second scan.
      jq -esr -f ${./semgrep-report.jq} "$output_dir/report.json" \
        > "$output_dir/count.txt"
      warnings=$(jq '.errors | length' "$output_dir/report.json")
      if [ "$warnings" -gt 0 ]; then
        echo "PARTIAL COVERAGE: $warnings source parsing warnings; findings are incomplete" \
          | tee "$output_dir/coverage.txt" >&2
      else
        echo 'Complete scan: no source parsing warnings' > "$output_dir/coverage.txt"
      fi
      cat "$output_dir/coverage.txt" > "$output_dir/report.txt"
      jq -r '.errors[] | .message' "$output_dir/report.json" >> "$output_dir/report.txt"
      jq -r '.results[] | "\(.path):\(.start.line): \(.check_id): \(.extra.message)"' \
        "$output_dir/report.json" >> "$output_dir/report.txt"
    '';
  };
in
mkSourceReport "semgrep" runner
