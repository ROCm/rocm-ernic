# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
# Normalize digit strings before arithmetic; never evaluate unbounded input.
normalize_pair_port() {
  local port="$1"
  case "$port" in
    ''|*[!0-9]*) echo 'ionic-vm-pair: --ssh-port needs a decimal number' >&2; return 2 ;;
  esac
  while [ "${#port}" -gt 1 ] && [ "${port:0:1}" = 0 ]; do port=${port:1}; done
  if [ "${#port}" -gt 5 ]; then
    echo 'ionic-vm-pair: --ssh-port is out of range (1-65534)' >&2
    return 2
  fi
  port=$((10#$port))
  if [ "$port" -lt 1 ] || [ "$port" -gt 65534 ]; then
    echo 'ionic-vm-pair: --ssh-port is out of range (1-65534)' >&2
    return 2
  fi
  echo "$port"
}
