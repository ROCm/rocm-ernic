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
# skips such a rule with only an entry in the JSON "errors" list, which is
# how two rules went unnoticed doing nothing.
{ pkgs, mkSourceReport }:

let
  rulesFile = ./semgrep-rules.yaml;
  testsDir = ./semgrep-tests;

  runner = pkgs.writeShellApplication {
    name = "run-semgrep-analysis";
    runtimeInputs = with pkgs; [ semgrep coreutils gnugrep cacert ];
    text = ''
      source_dir="$1"
      output_dir="$2"

      echo "=== semgrep Analysis ==="

      export SEMGREP_ENABLE_VERSION_CHECK=0
      export SEMGREP_SEND_METRICS=off
      export SSL_CERT_FILE="${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
      export OTEL_TRACES_EXPORTER=none
      # semgrep needs a writable HOME for its config/cache
      HOME="$(mktemp -d)"
      export HOME

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
        > "$output_dir/report.json" 2>&1 || true

      semgrep \
        --config ${rulesFile} \
        --metrics=off \
        --no-git-ignore \
        "$source_dir/src" \
        "$source_dir/third-party" \
        > "$output_dir/report.txt" 2>&1 || true

      if grep -q '"Rule parse error"' "$output_dir/report.json"; then
        grep -o '"message": "Rule parse error in rule [^:]*' \
          "$output_dir/report.json" >&2 || true
        echo "semgrep: a rule in semgrep-rules.yaml does not parse" >&2
        exit 1
      fi

      findings=$(grep -o '"check_id"' "$output_dir/report.json" | wc -l || echo "0")
      echo "$findings" > "$output_dir/count.txt"
    '';
  };
in
mkSourceReport "semgrep" runner
