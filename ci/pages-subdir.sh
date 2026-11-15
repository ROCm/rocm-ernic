#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# pages-subdir.sh
#
# Prints PAGES_SUBDIR from ci/pages.env, the project's directory on the
# gh-pages branch, or nothing for the branch root. Fails, printing nothing,
# unless the file has exactly one PAGES_SUBDIR line and its value has the
# form pages.env describes. docs-deploy.yml and .github/actions/publish-perf
# both read the setting through this, so they agree on where they write.

set -euo pipefail

env_file="$(dirname -- "${BASH_SOURCE[0]}")/pages.env"

# grep -c exits 1 when it counts 0; a missing file leaves count empty.
count=$(grep -c '^PAGES_SUBDIR=' -- "$env_file" || true)
if [ "$count" != 1 ]; then
  echo "$env_file: needs exactly one PAGES_SUBDIR= line" >&2
  exit 1
fi

subdir=$(sed -n 's/^PAGES_SUBDIR=//p' -- "$env_file")
component='[A-Za-z0-9][A-Za-z0-9_-]*'
if [ -n "$subdir" ] && ! [[ $subdir =~ ^$component(/$component)*$ ]]; then
  echo "$env_file: PAGES_SUBDIR=$subdir is not a relative path of" \
    "letters, digits, '-' and '_'" >&2
  exit 1
fi

printf '%s\n' "$subdir"
