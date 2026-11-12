#!/usr/bin/env bash
#
# tools/check-guest-image-tag.sh, in a scratch git repository holding a
# lockfile and a lane workflow: that it passes a tree whose guest and
# builder tags appear only in ci/guest-image.env and in workflow image:
# lines naming the pinned builder, and fails one with a tag anywhere
# else, naming the file and line.
#
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Usage: test_check_guest_image_tag.sh CHECK_SCRIPT
#
# Exit 0 = pass, exit 1 = fail.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 CHECK_SCRIPT" >&2
    exit 1
fi
SRC=$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")
[[ -f $SRC ]] || {
    echo "Usage: $0 CHECK_SCRIPT" >&2
    exit 1
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/test-check-guest-image-tag-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
trap 'exit 1' HUP INT TERM

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

pass() {
    echo "PASS: $*"
}

command -v git >/dev/null || fail "git is needed and not installed"

GUEST_TAG=20261024.g9d2be1e-vm.resolute-ionic-qm.c079195-qcow2
BUILDER=ghcr.io/rocm/rocm-ernic/ci-builder
BUILDER_TAG=qemu11.1.1-vfu.5b9c722-0123456789ab
OTHER_TAG=qemu11.1.1-vfu.5b9c722-ba9876543210

REPO="$WORK/repo"
mkdir -p "$REPO/ci" "$REPO/tools" "$REPO/.github/workflows" "$REPO/docs"
cp "$SRC" "$REPO/tools/check-guest-image-tag.sh"
cat >"$REPO/ci/guest-image.env" <<EOF
# The lockfile
GUEST_ARTIFACT_REPO=docker.io/example/guest
GUEST_ARTIFACT_TAG=$GUEST_TAG
BUILDER_IMAGE=$BUILDER
BUILDER_TAG=$BUILDER_TAG
EOF
cat >"$REPO/.github/workflows/lane.yml" <<EOF
jobs:
  a:
    container:
      image: $BUILDER:$BUILDER_TAG
  b:
    container:
        image:   $BUILDER:$BUILDER_TAG
EOF
cat >"$REPO/docs/ci.md" <<'EOF'
The lanes run in the builder ci/guest-image.env pins.
EOF
git -C "$REPO" init -q
git -C "$REPO" add -A
git -C "$REPO" -c user.name=test -c user.email=test@example.com \
    commit -q -m pins

# Run the check from the repository root, its output in $WORK/out and
# $WORK/err, and its exit status in $STATUS.
check() {
    STATUS=0
    (cd "$REPO" && tools/check-guest-image-tag.sh) \
        >"$WORK/out" 2>"$WORK/err" || STATUS=$?
}

# Check that the check fails, naming a line, then undo the change.
#   $1  what the change is, for the message
#   $2  the FILE:LINE it should name
#   $3  a command making the change, run in $REPO
fails_with() {
    (cd "$REPO" && eval "$3")
    git -C "$REPO" add -A
    check
    [[ $STATUS -eq 1 ]] ||
        fail "$1: exit status $STATUS, not 1: $(cat "$WORK/err")"
    grep -qF "  $2:" "$WORK/err" || fail "$1: $2 not named: $(cat "$WORK/err")"
    git -C "$REPO" reset -q --hard
    git -C "$REPO" clean -q -fd
}

check
[[ $STATUS -eq 0 ]] || fail "a clean tree failed: $(cat "$WORK/err")"
grep -qF "builder: $BUILDER:$BUILDER_TAG" "$WORK/out" ||
    fail "it didn't print the pins: $(cat "$WORK/out")"
pass "tags only in the lockfile and the lanes' image: lines pass"

fails_with "a guest tag in another file" docs/ci.md:2 \
    "echo 'pull $GUEST_TAG' >> docs/ci.md"
fails_with "a copy of the lockfile" some-lane/guest-image.env:3 \
    "mkdir some-lane && cp ci/guest-image.env some-lane/"
fails_with "a builder tag in another file" docs/ci.md:2 \
    "echo 'builder $BUILDER_TAG' >> docs/ci.md"
pass "a tag outside the lockfile fails"

fails_with "an image: line naming another builder tag" \
    .github/workflows/lane.yml:4 \
    "sed -i '4s/$BUILDER_TAG/$OTHER_TAG/' .github/workflows/lane.yml"
fails_with "the pinned builder in a comment" .github/workflows/lane.yml:8 \
    "echo '      # image: $BUILDER:$BUILDER_TAG' >> .github/workflows/lane.yml"
fails_with "the pinned builder in another key" .github/workflows/lane.yml:8 \
    "echo '      options: $BUILDER:$BUILDER_TAG' >> .github/workflows/lane.yml"
fails_with "the pinned builder's tag under another image" \
    .github/workflows/lane.yml:8 \
    "echo '      image: docker.io/x/ci-builder:$BUILDER_TAG' >> \
        .github/workflows/lane.yml"
fails_with "an image: line outside the workflows" docs/ci.md:2 \
    "echo 'image: $BUILDER:$BUILDER_TAG' >> docs/ci.md"
pass "only a workflow's image: line naming the pinned builder is allowed"

echo "pull $GUEST_TAG" >"$REPO/notes.md"
check
[[ $STATUS -eq 0 ]] || fail "an untracked file failed: $(cat "$WORK/err")"
rm "$REPO/notes.md"
pass "untracked files aren't searched"

# Check that a broken setup fails with status 2, saying why.
#   $1  what's broken, for the message
#   $2  what it should say
#   $3  a command breaking it, run in $REPO
setup_fails() {
    (cd "$REPO" && eval "$3")
    check
    [[ $STATUS -eq 2 ]] ||
        fail "$1: exit status $STATUS, not 2: $(cat "$WORK/err")"
    grep -qF -- "$2" "$WORK/err" || fail "$1: $(cat "$WORK/err")"
    git -C "$REPO" reset -q --hard
}

for key in GUEST_ARTIFACT_TAG BUILDER_IMAGE BUILDER_TAG; do
    setup_fails "no $key" "does not define $key" \
        "sed -i '/^$key=/d' ci/guest-image.env"
done
setup_fails "no lockfile" "not found" "rm ci/guest-image.env"

# Not even a checkout around it, wherever TMPDIR is
mv "$REPO/.git" "$WORK/dot-git"
GIT_CEILING_DIRECTORIES=$WORK check
[[ $STATUS -eq 2 ]] ||
    fail "outside a checkout: exit status $STATUS, not 2: $(cat "$WORK/err")"
grep -qF "git grep failed" "$WORK/err" ||
    fail "outside a checkout: $(cat "$WORK/err")"
pass "a missing lockfile, pin or git checkout fails with status 2"

echo "All guest image pin check tests passed."
