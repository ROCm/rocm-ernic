#!/usr/bin/env bash
#
# tools/check-guest-image-tag.sh, in a scratch git repository holding a
# lockfile and a lane workflow: that it passes a tree whose guest and
# builder tags appear only in ci/guest-image.env and in workflow image:
# lines naming the pinned builder, and fails one with a tag anywhere
# else, naming the file and line. All of it twice: with the project at
# the top of the repository, and in emulation/ernic/ of a repository
# holding another project, as in rocm-systems, where only the project and
# the workflows and actions named ernic-* are its.
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

# Each tag is built from parts, so no line of this file holds one: the
# check searches every tracked file, this one included
GUEST_TAG=resolute-k7.2.6-
GUEST_TAG+=0123456789ab
# The Docker Hub guest's tag, which the check still refuses
OLD_GUEST_TAG=20261024.g9d2be1e
OLD_GUEST_TAG+=-vm.resolute-ionic-qm.c079195-qcow2
BUILDER=ghcr.io/rocm/rocm-ernic/ci-builder
BUILDER_PREFIX=qemu11.1.1-vfu.5b9c722-
BUILDER_TAG=${BUILDER_PREFIX}0123456789ab
OTHER_TAG=${BUILDER_PREFIX}ba9876543210

# Make $REPO, a git repository holding the project in $REPO/$PROJ, with
# its lockfile, its check, a doc, and a lane workflow,
# .github/workflows/$LANE, all committed.
#   $1  the project's directory in the repository, ending in /, or
#       nothing for the top. In a directory, the lane is named ernic-*,
#       and another project's files and workflow hold tags of their own.
make_repo() {
    REPO="$WORK/repo"
    PROJ=$1
    LANE=.github/workflows/${PROJ:+ernic-}lane.yml
    rm -rf "$REPO"
    mkdir -p "$REPO/${PROJ}ci" "$REPO/${PROJ}tools" "$REPO/${PROJ}docs" \
        "$REPO/.github/workflows"
    cp "$SRC" "$REPO/${PROJ}tools/check-guest-image-tag.sh"
    cat >"$REPO/${PROJ}ci/guest-image.env" <<EOF
# The lockfile
GUEST_ARTIFACT_REPO=ghcr.io/rocm/rocm-ernic/guest-ionic
GUEST_ARTIFACT_TAG=$GUEST_TAG
BUILDER_IMAGE=$BUILDER
BUILDER_TAG=$BUILDER_TAG
EOF
    cat >"$REPO/$LANE" <<EOF
jobs:
  a:
    container:
      image: $BUILDER:$BUILDER_TAG
  b:
    container:
        image:   $BUILDER:$BUILDER_TAG
EOF
    cat >"$REPO/${PROJ}docs/ci.md" <<'EOF'
The lanes run in the builder ci/guest-image.env pins.
EOF
    if [[ -n $PROJ ]]; then
        mkdir -p "$REPO/other" "$REPO/.github/actions/other"
        echo "pull $GUEST_TAG" >"$REPO/other/notes.md"
        cat >"$REPO/.github/workflows/other.yml" <<EOF
jobs:
  a:
    container:
      image: $BUILDER:$OTHER_TAG
EOF
        echo "# builder $BUILDER_TAG" >"$REPO/.github/actions/other/action.yml"
    fi
    git -C "$REPO" init -q
    git -C "$REPO" add -A
    git -C "$REPO" -c user.name=test -c user.email=test@example.com \
        commit -q -m pins
}

# Run the check from outside the repository, its output in $WORK/out and
# $WORK/err, and its exit status in $STATUS.
check() {
    STATUS=0
    (cd "$WORK" && "$REPO/${PROJ}tools/check-guest-image-tag.sh") \
        >"$WORK/out" 2>"$WORK/err" || STATUS=$?
}

# Check that the check fails, naming a line, then undo the change.
#   $1  what the change is, for the message
#   $2  the FILE:LINE it should name, from the top of the repository
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

for proj in "" emulation/ernic/; do
    make_repo "$proj"
    where="the project at ${PROJ:-the top}"
    lock=${PROJ}ci/guest-image.env
    doc=${PROJ}docs/ci.md

    check
    [[ $STATUS -eq 0 ]] || fail "$where: a clean tree failed: $(cat "$WORK/err")"
    grep -qF "builder: $BUILDER:$BUILDER_TAG" "$WORK/out" ||
        fail "$where: it didn't print the pins: $(cat "$WORK/out")"
    pass "$where: tags only in the lockfile and the lanes' image: lines pass"

    fails_with "a guest tag in another file" "$doc:2" \
        "echo 'pull $GUEST_TAG' >> $doc"
    fails_with "a Docker Hub guest tag" "$doc:2" \
        "echo 'pull $OLD_GUEST_TAG' >> $doc"
    fails_with "a copy of the lockfile" "${PROJ}some-lane/guest-image.env:3" \
        "mkdir ${PROJ}some-lane && cp $lock ${PROJ}some-lane/"
    fails_with "a builder tag in another file" "$doc:2" \
        "echo 'builder $BUILDER_TAG' >> $doc"
    pass "$where: a tag outside the lockfile fails"

    fails_with "an image: line naming another builder tag" "$LANE:4" \
        "sed -i '4s/$BUILDER_TAG/$OTHER_TAG/' $LANE"
    fails_with "the pinned builder in a comment" "$LANE:8" \
        "echo '      # image: $BUILDER:$BUILDER_TAG' >> $LANE"
    fails_with "the pinned builder in another key" "$LANE:8" \
        "echo '      options: $BUILDER:$BUILDER_TAG' >> $LANE"
    fails_with "the pinned builder's tag under another image" "$LANE:8" \
        "echo '      image: docker.io/x/ci-builder:$BUILDER_TAG' >> $LANE"
    fails_with "an image: line outside the workflows" "$doc:2" \
        "echo 'image: $BUILDER:$BUILDER_TAG' >> $doc"
    pass "$where: only a workflow's image: line naming the pinned builder" \
        "is allowed"

    if [[ -n $PROJ ]]; then
        fails_with "a tag in one of the project's actions" \
            .github/actions/ernic-x/action.yml:1 \
            "mkdir .github/actions/ernic-x &&
                echo '# $GUEST_TAG' > .github/actions/ernic-x/action.yml"
        fails_with "an image: line in a workflow GitHub doesn't run" \
            "$PROJ.github/workflows/lane.yml:4" \
            "mkdir -p $PROJ.github/workflows &&
                cp $LANE $PROJ.github/workflows/lane.yml"
        pass "$where: the project's own workflows and actions are searched," \
            "and only those"
    fi

    echo "pull $GUEST_TAG" >"$REPO/${PROJ}notes.md"
    check
    [[ $STATUS -eq 0 ]] ||
        fail "$where: an untracked file failed: $(cat "$WORK/err")"
    rm "$REPO/${PROJ}notes.md"
    pass "$where: untracked files aren't searched"

    for key in GUEST_ARTIFACT_TAG BUILDER_IMAGE BUILDER_TAG; do
        setup_fails "$where: no $key" "does not define $key" \
            "sed -i '/^$key=/d' $lock"
    done
    setup_fails "$where: no lockfile" "not found" "rm $lock"

    # Not even a checkout around it, wherever TMPDIR is
    mv "$REPO/.git" "$WORK/dot-git"
    GIT_CEILING_DIRECTORIES=$WORK check
    [[ $STATUS -eq 2 ]] ||
        fail "$where: outside a checkout: exit status $STATUS, not 2:" \
            "$(cat "$WORK/err")"
    grep -qF "isn't in a git checkout" "$WORK/err" ||
        fail "$where: outside a checkout: $(cat "$WORK/err")"
    rm -rf "$WORK/dot-git"
    pass "$where: a missing lockfile, pin or git checkout fails with status 2"
done

echo "All guest image pin check tests passed."
