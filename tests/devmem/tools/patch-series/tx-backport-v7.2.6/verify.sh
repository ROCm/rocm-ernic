#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Apply patches/ernic-v7.2.6 to a fresh copy of v7.2.6 + srq + p4 with
# "git apply" (no repository, nothing committed), compare each step with
# the stages, parse each file with git mailinfo, and stage the DKMS layout
# of the result in dkms/patched (and of the base in dkms/base).
set -uo pipefail
R=${DEVMEM_ROOT:?set it to the work area}
B=$R/backport-scratch
P=$R/patches/ernic-v7.2.6
ETH=drivers/net/ethernet/pensando/ionic
RDMA=drivers/infiniband/hw/ionic
ts=$(date +%s)
mkdir -p "$B/old"
for d in tree mailinfo dkms; do [ -e "$B/$d" ] && mv "$B/$d" "$B/old/$d.$ts"; done
bash "$B/scripts/mkbase.sh" tree >/dev/null || exit 1
diff -rq "$B/base" "$B/tree" && echo "tree == base (v7.2.6 + srq + p4)"
mkdir -p "$B/mailinfo"
cd "$B/tree" || exit 1
n=0
for p in "$P"/0*.patch; do
	n=$((n + 1))
	b=$(basename "$p" .patch)
	echo "== $b"
	git apply --check --verbose "$p" 2>&1 | sed 's/^/   check: /'
	echo "   git apply --check rc=${PIPESTATUS[0]}"
	git apply --whitespace=error-all "$p"
	echo "   git apply --whitespace=error-all rc=$?"
	bad=0
	for f in "$B/stages/b$n/$ETH"/*; do
		cmp -s "$f" "$ETH/$(basename "$f")" || { bad=1; echo "   MISMATCH vs b$n: $f"; }
	done
	[ $bad = 0 ] && echo "   result identical to stage b$n"
	git mailinfo "$B/mailinfo/$b.msg" "$B/mailinfo/$b.diff" <"$p" >"$B/mailinfo/$b.info"
	echo "   git mailinfo rc=$? msg_lines=$(wc -l <"$B/mailinfo/$b.msg") diff_lines=$(wc -l <"$B/mailinfo/$b.diff")"
	sed 's/^/     /' "$B/mailinfo/$b.info"
	# what git am would apply is the diff mailinfo extracted
	git apply -R --check "$B/mailinfo/$b.diff" && echo "   mailinfo's diff is the diff just applied (reverse-applies)"
	grep -c '^Signed-off-by: Stephen' "$p" | sed 's/^/   Signed-off-by Stephen lines: /'
done
echo "== files changed against base"
diff -rq "$B/base" "$B/tree"
echo "== split with git mailsplit (what git am does first)"
mkdir -p "$B/mailinfo/split"
for p in "$P"/0*.patch; do
	mkdir -p "$B/mailinfo/split/$(basename "$p" .patch)"
	git mailsplit -o"$B/mailinfo/split/$(basename "$p" .patch)" "$p" | sed "s/^/   $(basename "$p"): messages=/"
done

# DKMS layout, as setup-ionic-dkms.sh stages it
for v in base:base patched:tree; do
	d=$B/dkms/${v%%:*}
	t=$B/${v##*:}
	mkdir -p "$d/eth" "$d/rdma" "$d/uapi/rdma"
	cp -r "$t/$ETH/." "$d/eth/"
	cp -r "$t/$RDMA/." "$d/rdma/"
	cp "$t/include/uapi/rdma/ionic-abi.h" "$d/uapi/rdma/"
	# The Kbuild is the heredoc of setup-ionic-dkms.sh, extracted verbatim.
	sed -n "/tee \"\${DKMS_TREE_DIR}\/Kbuild\"/,/^EOF\$/p" \
		"${ERNIC_REPO:-$HOME/Projects/rocm-ernic}/ansible/files/scripts/setup-ionic-dkms.sh" |
		sed -e 1d -e '$d' >"$d/Kbuild"
done
echo "== Kbuild staged:"
cat "$B/dkms/patched/Kbuild"
