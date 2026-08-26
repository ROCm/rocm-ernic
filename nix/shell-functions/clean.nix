# nix/shell-functions/clean.nix
#
# Clean-related dev-shell functions. Returns a bash snippet spliced into
# devshell.nix's shellHook.
{ }:
''
  # Remove the CMake build tree.
  ernic-clean() {
    local root
    root=$(git rev-parse --show-toplevel 2>/dev/null || echo .)
    rm -rf "$root/build"
    echo "Removed $root/build"
  }
''
