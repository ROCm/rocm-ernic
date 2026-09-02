# nix/shell-functions/build.nix
#
# Build-related dev-shell functions for rocm-ernic. Returns a bash snippet
# that is spliced into devshell.nix's shellHook.
{ }:
''
  # Configure the CMake build tree (Ninja generator, compile DB on).
  # ERNIC_WERROR defaults OFF here: gcc 15 / clang emit warnings on the
  # QEMU-ported sources that -Werror would turn fatal. Re-enable with
  #   ernic-configure -DERNIC_WERROR=ON
  # Extra args are forwarded to cmake, e.g.:
  #   ernic-configure -DERNIC_USE_SANITIZERS=ON
  ernic-configure() {
    local root
    root=$(git rev-parse --show-toplevel 2>/dev/null || echo .)
    cmake -S "$root" -B "$root/build" -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DERNIC_WERROR=OFF \
      "$@"
  }

  # Build the project. Configures first if the build tree is missing.
  ernic-build() {
    local root
    root=$(git rev-parse --show-toplevel 2>/dev/null || echo .)
    if [ ! -d "$root/build" ]; then
      ernic-configure || return 1
    fi
    cmake --build "$root/build" "$@"
  }

  # Run the CTest suite.
  ernic-test() {
    local root
    root=$(git rev-parse --show-toplevel 2>/dev/null || echo .)
    ctest --test-dir "$root/build" --output-on-failure "$@"
  }
''
