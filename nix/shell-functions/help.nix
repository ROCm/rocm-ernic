# nix/shell-functions/help.nix
#
# Help text for the rocm-ernic dev shell. Returns a bash snippet spliced
# into devshell.nix's shellHook.
{ }:
''
  ernic-help() {
    cat <<'EOF'
  rocm-ernic dev shell — available commands:

    ernic-configure [cmake args]  Configure ./build (Ninja, compile DB on)
    ernic-build     [cmake args]  Build (auto-configures if needed)
    ernic-test      [ctest args]  Run the CTest suite
    ernic-clean                   Remove ./build
    ernic-help                    Show this help

  Sanitizer builds:
    ernic-configure -DERNIC_USE_SANITIZERS=ON        # ASAN+LSAN+UBSAN
    ernic-configure -DERNIC_USE_THREAD_SANITIZER=ON  # TSAN

  Nix build targets (from the repo root):
    nix build .#rocm-ernic        Build the server binary
    nix develop                   Enter this shell

  Analysis targets are added in later phases; run `nix flake show`
  to list everything currently available.
EOF
  }
''
