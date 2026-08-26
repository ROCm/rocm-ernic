# nix/analysis/sanitizers.nix
#
# Instrumented builds that exercise the server under sanitizers on the
# loopback backend. Two variants (mutually exclusive per
# cmake/ErnicSanitizers.cmake):
#   sanitizers        ASan + LSan + UBSan  (-DERNIC_USE_SANITIZERS=ON)
#   thread-sanitizer  TSan                 (-DERNIC_USE_THREAD_SANITIZER=ON)
#
# The build never fails on a violation — it captures whatever the
# sanitizer runtime reports and records a count, so the target always
# produces a report.
#
# Note on LSan: the leak check runs via ptrace at process exit. Some
# sandboxes restrict ptrace; if so, LSan prints a fatal error instead of a
# leak list. ASan/UBSan (the memory-safety and UB checks that fire during
# the run) are unaffected, and valgrind.nix provides a ptrace-free leak
# detector as a second opinion.
{ lib, pkgs, src, libvfio-user }:

let
  packages = import ../packages.nix { inherit pkgs libvfio-user; };
  compatCflags = import ../compat-cflags.nix { };
  driver = import ./run-loopback.nix { inherit pkgs; };

  mkSanBuild = { name, cmakeFlag, markerGrep, description }:
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
          ${cmakeFlag}
        cmake --build "$srcTop/build"
        runHook postBuild
      '';

      doCheck = true;
      checkPhase = ''
        runHook preCheck
        echo "=== Exercising server under ${name} ==="

        # Collect (don't abort on) all diagnostics.
        export ASAN_OPTIONS="detect_leaks=1:halt_on_error=0:abort_on_error=0:exitcode=0:print_stats=0"
        export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0:abort_on_error=0"
        export TSAN_OPTIONS="halt_on_error=0:exitcode=0"

        export OUT_LOG="$NIX_BUILD_TOP/server.log"
        export CLIENT_LOG="$NIX_BUILD_TOP/client.log"

        bash ${driver} \
          "$srcTop/build/rocm-ernic" \
          "$srcTop/build/tests/test_pci_client" || true

        echo "--- server log tail ---"
        tail -n 40 "$OUT_LOG" || true
        runHook postCheck
      '';

      installPhase = ''
        mkdir -p $out

        server_log="$NIX_BUILD_TOP/server.log"
        touch "$server_log"

        # Strip store prefixes so any file paths in the report are readable.
        sed 's|/nix/store/[a-z0-9]\{32\}-[^/]*/||g' "$server_log" > $out/report.txt

        violations=$(grep -cE '${markerGrep}' "$server_log" || true)
        [ -z "$violations" ] && violations=0
        echo "$violations" > $out/count.txt

        cp "$NIX_BUILD_TOP/client.log" $out/client.log 2>/dev/null || true

        {
          echo "=== ${description} ==="
          echo ""
          echo "Server exercised on the loopback backend with the PCI-config"
          echo "test client, then shut down cleanly (SIGTERM)."
          echo ""
          if [ "$violations" -gt 0 ]; then
            echo "Result: $violations sanitizer report line(s) — see report.txt."
          else
            echo "Result: no sanitizer violations detected."
          fi
        } > $out/summary.txt
        cat $out/summary.txt
      '';

      dontFixup = true;
    };
in
{
  sanitizers = mkSanBuild {
    name = "sanitizers";
    cmakeFlag = "-DERNIC_USE_SANITIZERS=ON";
    description = "ASan + LSan + UBSan";
    markerGrep = "ERROR: (Address|Leak)Sanitizer|runtime error:|SUMMARY: (Address|Undefined|Leak)|detected memory leaks";
  };

  thread-sanitizer = mkSanBuild {
    name = "thread-sanitizer";
    cmakeFlag = "-DERNIC_USE_THREAD_SANITIZER=ON";
    description = "ThreadSanitizer";
    markerGrep = "WARNING: ThreadSanitizer|SUMMARY: ThreadSanitizer";
  };
}
