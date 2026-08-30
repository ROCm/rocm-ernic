# nix/shell-functions/ascii-art.nix
#
# ASCII banner printed on entry to `nix develop`. Static art (no jp2a / image
# dependency, unlike the xdp2 reference it mirrors). Returns a bash snippet
# spliced into devshell.nix's welcome.
#
# Each line is a single-quoted argument to `printf`, so the shell reproduces
# the backslashes verbatim (no heredoc / indentation pitfalls); the format
# wraps every line in green and resets after.
{ }:
''
  printf '\033[0;32m%s\033[0m\n' \
    '      ____   ___   ____ __  __' \
    '    |  _ \ / _ \ / ___|  \/  |' \
    '    | |_) | | | | |   | |\/| |' \
    '    |  _ <| |_| | |___| |  | |' \
    '    |_| \_\\___/ \____|_|  |_|' \
    "" \
    '              +-------+' \
    '       GPU ===| ERNIC |=== RoCEv2 ===>' \
    '              +-------+' \
    '          RDMA over Ethernet'
''
