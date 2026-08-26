# nix/analysis/fuzz.nix
#
# libFuzzer harnesses for the untrusted wire-format parsers. Two targets:
#   fuzz       build the fuzzer binaries + seed corpora + a run-fuzzers
#              wrapper (does not run them)
#   fuzz-run   build, then run each harness for a bounded time, saving any
#              crash inputs and a symbolized report (always exits 0 — the
#              crashes are the deliverable, not a build failure)
#
# Harnesses target the genuinely self-contained parsers:
#   fuzz_rdma_cm_proto   rdma_cm_process_message() — pure TCP-payload parser
#   fuzz_dhcp_server     dhcp_server_process()      — DHCP packet parser
#   fuzz_net_headers     parse_eth/ip/tcp/udp + checksums (net_headers.h)
#
# Deliberately NOT fuzzed here: eth_rx_inject_frame() and the pvrdma_cmd
# handlers. Those are DMA/ring plumbing — they memcpy guest data through
# rdma_pci_dma_map() and manipulate the RDMA resource manager, so a faithful
# harness needs a fully-wired PVRDMADev (PCI + DMA + backend) fixture rather
# than a byte buffer. Reaching them meaningfully is a device-emulation
# harness, tracked as future work; see nix/analysis/fuzz/README.md.

{ lib, pkgs, src, libvfio-user }:

let
  packages = import ../packages.nix { inherit pkgs libvfio-user; };
  compat = import ../compat-cflags.nix { };

  includeFlags = lib.concatStringsSep " " [
    "-Isrc"
    "-Isrc/from-qemu"
    "-Isrc/from-qemu/utils"
    "-Isrc/from-qemu/include/qemu-extra"
  ];
  sanFlags = "-fsanitize=fuzzer,address,undefined -g -O1 -fno-omit-frame-pointer";

  symbolizer = "${lib.getBin pkgs.llvm}/bin/llvm-symbolizer";

  # name -> extra .c files linked alongside nix/analysis/fuzz/fuzz_<name>.c
  harnesses = [
    { name = "rdma_cm_proto";
      extra = [
        "src/from-qemu/utils/rdma_cm_proto.c"
        "src/from-qemu/utils/error-report.c"
      ]; }
    { name = "dhcp_server";
      extra = [
        "src/from-qemu/utils/dhcp_server.c"
        "src/from-qemu/utils/error-report.c"
      ]; }
    { name = "net_headers";
      extra = [ ]; }
  ];

  names = map (h: h.name) harnesses;

  compileLines = lib.concatMapStringsSep "\n" (h: ''
    echo "Building fuzz_${h.name}..."
    clang ${sanFlags} ${includeFlags} -D_GNU_SOURCE \
      "nix/analysis/fuzz/fuzz_${h.name}.c" \
      ${lib.concatStringsSep " " (map (f: "\"${f}\"") h.extra)} \
      $GLIB \
      -o "out/bin/fuzz_${h.name}"
    # A trivial seed so each corpus starts non-empty.
    mkdir -p "out/corpus/${h.name}"
    printf '\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0' > "out/corpus/${h.name}/seed0"
  '') harnesses;

  runFuzzers = pkgs.writeShellScript "run-fuzzers" ''
    # Run each fuzzer for FUZZ_TIME seconds (default 60). Crash inputs and
    # per-harness logs land in FUZZ_WORKDIR (default: a temp dir). Never
    # fails on a crash — the caller inspects the workdir.
    set -u
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

  fuzzers = pkgs.stdenv.mkDerivation {
    pname = "rocm-ernic-fuzzers";
    version = "0.2.0";
    inherit src;

    nativeBuildInputs = [ pkgs.clang pkgs.pkg-config ];
    buildInputs = [ pkgs.glib ];

    dontConfigure = true;
    env.NIX_CFLAGS_COMPILE = compat.string;

    buildPhase = ''
      runHook preBuild
      GLIB=$(pkg-config --cflags --libs glib-2.0)
      mkdir -p out/bin out/corpus
      ${compileLines}

      install -m755 ${runFuzzers} out/bin/run-fuzzers
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
    version = "0.2.0";
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
