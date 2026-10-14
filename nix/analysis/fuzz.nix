# nix/analysis/fuzz.nix
#
# libFuzzer harnesses for the untrusted wire-format parsers. Two targets:
#   fuzz       build the fuzzer binaries + seed corpora + a run-fuzzers
#              wrapper (does not run them)
#   fuzz-run   build, then run each harness for a bounded time, saving any
#              crash inputs and a symbolized report (always exits 0 — the
#              crashes are the deliverable, not a build failure)
#
# Harnesses:
#   fuzz_rdma_cm_proto   rdma_cm_process_message() — pure TCP-payload parser
#   fuzz_net_headers     parse_eth/ip/tcp/udp + checksums (net_headers.h)
#   fuzz_profile_parse   ionic_profile_apply_text() — the --nic-profile ini
#                        loader
#   fuzz_eth_classify    ionic_emu_mac_class() + ionic_eth_flow_hash() — the
#                        per-frame MAC class and Rx steering hash
#   fuzz_ionic_stats     the ionic STATS_HDRS/VALS admin commands, through
#                        dispatch_wqe() over the fake device shared with
#                        tests/test_ionic_stats.c (tests/ionic_adminq_fakes.h)
#   fuzz_fault_parse     ionic_fault_parse() — the --inject fault spec parser
#
# Not fuzzed yet: the rest of the ionic devcmd and datapath handlers. They
# manipulate the RDMA resource manager and the rings, so they need more of
# the device faked than the stats commands do; see nix/analysis/fuzz/README.md.

{ ctx, src }:

let
  inherit (ctx) pkgs lib compat version;
  libvfioUser = ctx."libvfio-user";

  includeFlags = lib.concatStringsSep " " [
    "-Isrc"
    "-Isrc/net"
    "-Isrc/qemu-compat/include"
    "-Ithird-party/qemu/hw/rdma"
  ];
  sanFlags = "-fsanitize=fuzzer,address,undefined -g -O1 -fno-omit-frame-pointer";

  symbolizer = "${lib.getBin pkgs.llvm}/bin/llvm-symbolizer";

  # name -> extra .c files linked alongside nix/analysis/fuzz/fuzz_<name>.c,
  # optional extra compiler flags, and whether the harness writes its own
  # seed corpus (FUZZ_EMIT_SEEDS, from the seed table it checks at startup).
  #
  # These targets are opt-in, so tests/nix/CMakeLists.txt also builds every
  # harness against a plain main() during the ordinary CMake build to keep
  # them from going stale. Add a harness in both places.
  harnesses = [
    { name = "rdma_cm_proto";
      extra = [
        "src/net/rdma_cm_proto.c"
        "src/qemu-compat/error-report.c"
      ]; }
    { name = "net_headers";
      extra = [ ]; }
    { name = "profile_parse";
      extra = [ "src/ionic_profile.c" "src/ionic_profiles.c" ];
      seeds = true; }
    { name = "eth_classify";
      extra = [ "src/ionic_emu_stats.c" "src/ionic_eth_steer.c" ];
      seeds = true; }
    { name = "ionic_stats";
      extra = [ "src/ionic_fault.c" ];
      cflags = "-Itests -I${libvfioUser}/include";
      seeds = true; }
    { name = "fault_parse";
      extra = [ "src/ionic_fault.c" ];
      seeds = true; }
  ];

  names = map (h: h.name) harnesses;

  compileLines = lib.concatMapStringsSep "\n" (h: ''
    echo "Building fuzz_${h.name}..."
    clang ${sanFlags} ${includeFlags} ${h.cflags or ""} -D_GNU_SOURCE \
      "nix/analysis/fuzz/fuzz_${h.name}.c" \
      ${lib.concatStringsSep " " (map (f: "\"${f}\"") h.extra)} \
      $GLIB \
      -o "out/bin/fuzz_${h.name}"
    # A trivial seed so each corpus starts non-empty.
    mkdir -p "out/corpus/${h.name}"
    printf '\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0' > "out/corpus/${h.name}/seed0"
  '' + lib.optionalString (h.seeds or false) ''
    # The harness checks its seed table at startup and writes it out as a
    # corpus; a failing seed fails the build.
    FUZZ_EMIT_SEEDS="out/corpus/${h.name}" "out/bin/fuzz_${h.name}" -runs=0
  '') harnesses;

  # writeShellApplication so it is shellcheck-checked and self-contained
  # (coreutils/findutils on PATH). bashOptions is limited to nounset so the
  # "run each harness, never fail on a crash" behaviour is preserved.
  runFuzzers = pkgs.writeShellApplication {
    name = "run-fuzzers";
    runtimeInputs = [ pkgs.coreutils pkgs.findutils ];
    bashOptions = [ "nounset" ];
    text = ''
    # Run each fuzzer for FUZZ_TIME seconds (default 60). Crash inputs and
    # per-harness logs land in FUZZ_WORKDIR (default: a temp dir). Never
    # fails on a crash — the caller inspects the workdir.
    root="$(cd "$(dirname "$0")/.." && pwd)"
    time="''${FUZZ_TIME:-60}"
    workdir="''${FUZZ_WORKDIR:-$(mktemp -d)}"
    export ASAN_SYMBOLIZER_PATH="${symbolizer}"
    export UBSAN_SYMBOLIZER_PATH="${symbolizer}"
    export ASAN_OPTIONS="abort_on_error=0:detect_leaks=0:symbolize=1"

    echo "Fuzzing for ''${time}s each; workdir=$workdir"
    for name in ${lib.concatStringsSep " " names}; do
      echo "=== fuzz_$name ==="
      mkdir -p "$workdir/$name"
      cp "$root/corpus/$name"/* "$workdir/$name/" 2>/dev/null || true
      "$root/bin/fuzz_$name" \
        -max_total_time="$time" \
        -artifact_prefix="$workdir/$name/" \
        "$workdir/$name" \
        > "$workdir/$name.log" 2>&1 || true
      crashes=$(find "$workdir/$name" -name 'crash-*' 2>/dev/null | wc -l)
      echo "  crashes: $crashes (log: $workdir/$name.log)"
    done
    echo "$workdir"
    '';
  };

  fuzzers = pkgs.stdenv.mkDerivation {
    pname = "rocm-ernic-fuzzers";
    inherit version src;

    nativeBuildInputs = [ pkgs.clang pkgs.pkg-config ];
    buildInputs = [ pkgs.glib ];

    dontConfigure = true;
    env.NIX_CFLAGS_COMPILE = compat.string;

    buildPhase = ''
      runHook preBuild
      GLIB=$(pkg-config --cflags --libs glib-2.0)
      mkdir -p out/bin out/corpus
      ${compileLines}

      install -m755 ${lib.getExe runFuzzers} out/bin/run-fuzzers
      runHook postBuild
    '';

    installPhase = ''
      mkdir -p $out
      cp -r out/* $out/
    '';

    dontFixup = true;
  };

  # Bounded run: exercise every harness briefly, collect crashes + a
  # symbolized reproduction for each, and summarise. Always succeeds.
  runSeconds = 20;

  fuzz-run = pkgs.stdenv.mkDerivation {
    pname = "rocm-ernic-fuzz-run";
    inherit version;
    dontUnpack = true;
    dontConfigure = true;

    buildPhase = ''
      runHook preBuild
      export FUZZ_TIME=${toString runSeconds}
      export FUZZ_WORKDIR="$PWD/work"
      mkdir -p "$FUZZ_WORKDIR"
      ${fuzzers}/bin/run-fuzzers || true
      runHook postBuild
    '';

    installPhase = ''
      mkdir -p $out
      work="$PWD/work"

      total=0
      {
        echo "=== Fuzzing summary (${toString runSeconds}s per harness) ==="
        echo ""
        for name in ${lib.concatStringsSep " " names}; do
          n=$(find "$work/$name" -name 'crash-*' 2>/dev/null | wc -l)
          total=$((total + n))
          printf '  %-16s %s crash input(s)\n' "$name:" "$n"

          if [ -f "$work/$name.log" ]; then
            cp "$work/$name.log" "$out/$name.log"
          fi

          # Save crash inputs and a symbolized reproduction per harness.
          first_crash=$(find "$work/$name" -name 'crash-*' 2>/dev/null | head -1)
          if [ -n "$first_crash" ]; then
            mkdir -p "$out/crashes/$name"
            cp "$work/$name"/crash-* "$out/crashes/$name/" 2>/dev/null || true
            ASAN_SYMBOLIZER_PATH="${symbolizer}" \
            ASAN_OPTIONS="symbolize=1:detect_leaks=0:abort_on_error=0" \
              ${fuzzers}/bin/fuzz_$name "$first_crash" \
              > "$out/crashes/$name/repro.txt" 2>&1 || true
          fi
        done
        echo ""
        echo "Total crash inputs: $total"
        echo ""
        echo "Crash inputs + symbolized repros under crashes/<harness>/."
        echo "Full logs: <harness>.log."
      } > $out/summary.txt

      echo "$total" > $out/count.txt
      cat $out/summary.txt
    '';

    dontFixup = true;
  };
in
{
  inherit fuzzers fuzz-run;
}
