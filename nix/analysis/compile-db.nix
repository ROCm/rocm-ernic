# nix/analysis/compile-db.nix
#
# Produce a self-contained compilation database for the static analysers.
#
# rocm-ernic uses CMake, so `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` emits a
# compile_commands.json directly — no build-log parsing (unlike the xdp2
# reference, which had to scrape `make V=1`). We only need to *configure*
# (not build): there is no code-generation step, so the pristine sources
# plus the compile DB are enough for clang-tidy / cppcheck.
#
# The output is a store path containing:
#   $out/                        a copy of the source tree (src/, CMakeLists…)
#   $out/compile_commands.json   with the sandbox build prefix rewritten to
#                                $out, so every 'file' and -I path is readable
#                                and immutable when the analysers consume it.
#
# The source sits at $out root (not $out/source) so that triage's
# normalize_path — which strips /nix/store/<hash>-<name>/ — yields repo-
# relative paths like src/rocm_ernic_compat.c, which its filters expect.

{ pkgs, lib, libvfio-user }:

let
  packages = import ../packages.nix { inherit pkgs libvfio-user; };
  compatCflags = import ../compat-cflags.nix { };
in
pkgs.stdenv.mkDerivation {
  pname = "rocm-ernic-compile-db";
  version = "0.2.0";

  src = ../../.;

  inherit (packages) nativeBuildInputs buildInputs;

  # Let us drive cmake by hand (configure only) instead of the setup hook.
  dontUseCmakeConfigure = true;

  env.NIX_CFLAGS_COMPILE = compatCflags.string;

  buildPhase = ''
    runHook preBuild
    srcTop="$PWD"
    cmake -S "$srcTop" -B "$srcTop/build" -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DERNIC_WERROR=OFF
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$out"
    # Copy the source tree, then replace the transient build dir with an
    # empty one so the DB's 'directory' field resolves for clang-tidy.
    cp -r "$srcTop"/. "$out/"
    rm -rf "$out/build"
    mkdir -p "$out/build"

    # Rewrite the sandbox source root to the store copy. The build dir's
    # compile_commands.json was removed with it, so write a fresh one.
    sed "s|$srcTop|$out|g" \
      "$srcTop/build/compile_commands.json" > "$out/compile_commands.json"

    echo "compile_commands.json entries: $(${pkgs.jq}/bin/jq length "$out/compile_commands.json")"
    runHook postInstall
  '';

  dontFixup = true;
  doCheck = false;
}
