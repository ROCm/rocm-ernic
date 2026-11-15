#!/usr/bin/env bash
#
# The project in a directory of a larger repository, as it will be in
# rocm-systems: a copy of the files git tracks in SOURCE_DIR, put in
# emulation/ernic/ of a scratch repository, with its workflows and actions
# moved to .github/ at the top and named ernic-*, beside another project
# whose files and workflows every check would refuse. Checks that
#   - tools/check-guest-image-tag.sh, ci/images/image-tag.sh --check and
#     tools/check-embedded-shell.py pass, run from outside the repository,
#     so they ignore the other project;
#   - each still finds a problem in the project's own workflows and
#     actions, so they haven't stopped looking at .github/;
#   - the project configures and builds there, with the commit's SHA.
#
# The copy is of the working tree, so uncommitted changes to tracked files
# are tested, but untracked files, such as notes, aren't copied.
#
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Usage: test_nested_checkout.sh SOURCE_DIR [CMAKE_ARG...]
#   SOURCE_DIR  a git checkout of the project
#   CMAKE_ARG   passed to the configure, e.g. -G Ninja,
#               -DCMAKE_PREFIX_PATH=...
#
# Needs git, cmake, ShellCheck, and Python with PyYAML.
#
# Exit 0 = pass, exit 1 = fail.

set -euo pipefail

if [[ $# -lt 1 || ! -d $1 ]]; then
    echo "Usage: $0 SOURCE_DIR [CMAKE_ARG...]" >&2
    exit 1
fi
SRC=$(cd -- "$1" && pwd)
shift

WORK=$(mktemp -d "${TMPDIR:-/tmp}/test-nested-checkout-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
trap 'exit 1' HUP INT TERM

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

pass() {
    echo "PASS: $*"
}

for tool in git cmake shellcheck python3; do
    command -v "$tool" >/dev/null || fail "$tool is needed and not installed"
done
python3 -c 'import yaml' 2>/dev/null || fail "PyYAML is needed and not installed"
git -C "$SRC" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    fail "$SRC isn't a git checkout"

TOP="$WORK/top"
PROJ=emulation/ernic
DIR="$TOP/$PROJ"

# ── the scratch repository ──────────────────────

# The files git tracks, from the working tree, less any deleted there
mkdir -p "$DIR"
git -C "$SRC" ls-files -z |
    while IFS= read -r -d '' f; do
        if [[ -e $SRC/$f || -L $SRC/$f ]]; then
            printf '%s\0' "$f"
        fi
    done |
    tar -C "$SRC" --null -T - -cf - | tar -C "$DIR" -xf -

# The workflows and actions, at the top and named ernic-*
mv "$DIR/.github" "$TOP/.github"
for f in "$TOP"/.github/workflows/* "$TOP"/.github/actions/*; do
    name=${f##*/}
    if [[ $name != ernic-* ]]; then
        mv "$f" "${f%/*}/ernic-$name"
    fi
done

# Another project's files, which hold what each check refuses: a guest
# tag, a stale builder in a workflow's image: line, and shell with
# ShellCheck findings in a workflow run under sh, an action, and a
# heredoc. The tag is built from parts, so no line of this file holds one.
GUEST_TAG=resolute-k7.2.6-
GUEST_TAG+=0123456789ab
mkdir -p "$TOP/other" "$TOP/.github/actions/other"
echo "pull $GUEST_TAG" >"$TOP/other/notes.md"
cat >"$TOP/other/run" <<'EOF'
#!/usr/bin/env bash
bash -s <<'EOS'
echo $1
EOS
EOF
chmod +x "$TOP/other/run"
cat >"$TOP/.github/workflows/other.yml" <<'EOF'
on: push
jobs:
  a:
    runs-on: ubuntu-latest
    container:
      image: ghcr.io/rocm/rocm-ernic/ci-builder:stale
    steps:
      - run: '[[ -n $1 ]] && echo $1'
EOF
echo "# guest $GUEST_TAG" >>"$TOP/.github/workflows/other.yml"
cat >"$TOP/.github/actions/other/action.yml" <<'EOF'
runs:
  using: composite
  steps:
    - shell: bash
      run: echo $1
EOF

git -C "$TOP" init -q
git -C "$TOP" add -A
git -C "$TOP" -c user.name=test -c user.email=test@example.com \
    commit -q -m nested

# ── the checks ──────────────────────────────────

# Run a check from outside the repository, its output in $WORK/out and
# its exit status in $STATUS.
#   $@  the check, from the project's directory, and its arguments
run_check() {
    STATUS=0
    (cd "$WORK" && "$DIR/$1" "${@:2}") >"$WORK/out" 2>&1 || STATUS=$?
}

# Check that a check passes.
#   $@  the check and its arguments
passes() {
    run_check "$@"
    [[ $STATUS -eq 0 ]] || fail "$*: exit status $STATUS: $(cat "$WORK/out")"
}

# Check that a check fails, naming a file, then undo the change.
#   $1  what the change is, for the message
#   $2  what it should name, from the top of the repository
#   $3  a command making the change, run at the top of the repository
#   $@  the check and its arguments
fails_with() {
    local what=$1 name=$2 change=$3
    shift 3
    (cd "$TOP" && eval "$change")
    git -C "$TOP" add -A
    run_check "$@"
    [[ $STATUS -eq 1 ]] ||
        fail "$*, $what: exit status $STATUS, not 1: $(cat "$WORK/out")"
    grep -qF -- "$name" "$WORK/out" ||
        fail "$*, $what: $name not named: $(cat "$WORK/out")"
    git -C "$TOP" reset -q --hard
    git -C "$TOP" clean -q -fd
}

passes tools/check-guest-image-tag.sh
passes ci/images/image-tag.sh --check
passes tools/check-embedded-shell.py
pass "the checks pass, and ignore another project's files"

passes tools/check-embedded-shell.py --list
for what in .github/workflows/ernic- .github/actions/ernic- \
    "$PROJ/ansible/" "$PROJ/tests/"; do
    grep -qF -- "$what" "$WORK/out" ||
        fail "check-embedded-shell.py found nothing in $what*:" \
            "$(cat "$WORK/out")"
done
grep -qE '^(other/|\.github/(workflows|actions)/other)' "$WORK/out" &&
    fail "check-embedded-shell.py found another project's shell:" \
        "$(cat "$WORK/out")"

# The builder pinned in the lanes' image: lines
builder=$(sed -n 's/^BUILDER_IMAGE=//p' "$DIR/ci/guest-image.env")
lane=$(grep -lE "^[[:space:]]*image:[[:space:]]+$builder:" \
    "$TOP"/.github/workflows/ernic-*.yml | head -n 1)
[[ -n $lane ]] || fail "no ernic-* workflow pins $builder"
lane=${lane#"$TOP/"}

fails_with "a guest tag in one of the project's workflows" "$lane:" \
    "echo '# $GUEST_TAG' >> $lane" \
    tools/check-guest-image-tag.sh
fails_with "a stale builder in one of the project's workflows" "$lane:" \
    "sed -i -E 's|(image:[[:space:]]+$builder:).*|\\1stale|' $lane" \
    ci/images/image-tag.sh --check
fails_with "a ShellCheck finding in one of the project's actions" \
    .github/actions/ernic-x/action.yml \
    "mkdir .github/actions/ernic-x &&
        cp .github/actions/other/action.yml .github/actions/ernic-x/" \
    tools/check-embedded-shell.py
pass "the checks look at the project's workflows and actions"

# ── the build ───────────────────────────────────

cmake -S "$DIR" -B "$WORK/build" "$@" >"$WORK/out" 2>&1 ||
    fail "the configure failed: $(cat "$WORK/out")"
grep -qx "ERNIC_GIT_SHA_FULL:STRING=$(git -C "$TOP" rev-parse HEAD)" \
    "$WORK/build/CMakeCache.txt" ||
    fail "the build doesn't name the repository's commit:" \
        "$(grep ERNIC_GIT_SHA "$WORK/build/CMakeCache.txt")"
cmake --build "$WORK/build" >"$WORK/out" 2>&1 ||
    fail "the build failed: $(tail -n 50 "$WORK/out")"
pass "the project configures and builds, naming the repository's commit"

echo "All nested checkout tests passed."
