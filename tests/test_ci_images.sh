#!/usr/bin/env bash
#
# ci/images/image-tag.sh, ci/images/ensure-image.sh and
# ci/images/guest/push.sh, on a copy of ci/images/ in a scratch git
# repository: that the tag names exactly the files that define an image,
# wherever the directory is; that ensure-image.sh builds and pushes only
# an image the registry says it hasn't got, and never one that fails its
# checks; and that push.sh tags the guest only once its metadata is
# attached. Stand-ins for docker, oras, zstd and the guest's build, check
# and push scripts record what they're asked to do; nothing is built.
#
# Everything runs on the copy, so IMAGES_DIR needn't be in a git checkout,
# as the nightly build's isn't. git and jq are needed.
#
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Usage: test_ci_images.sh IMAGES_DIR
#
# Exit 0 = pass, exit 1 = fail.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 IMAGES_DIR" >&2
    exit 1
fi
SRC=$(cd -- "$1" && pwd)

WORK=$(mktemp -d "${TMPDIR:-/tmp}/test-ci-images-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
trap 'exit 1' HUP INT TERM

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

pass() {
    echo "PASS: $*"
}

for tool in git jq; do
    command -v "$tool" >/dev/null || fail "$tool is needed and not installed"
done

# The lockfile's lines for a stale guest
STALE_GUEST="GUEST_ARTIFACT_REPO=ghcr.io/rocm/rocm-ernic/guest-ionic
GUEST_ARTIFACT_TAG=stale
GUEST_ARTIFACT_INFO_TYPE=application/x-stale
GUEST_ARTIFACT_DISK=stale.qcow2
GUEST_ARTIFACT_USER=stale"

# A git repository holding a copy of ci/images/, the IONIC_KERNEL_REF the
# guest's kernel must be, and a lockfile and a workflow pinning a stale
# builder and guest, all committed
#   $1  where
make_repo() {
    mkdir -p "$1/ci" "$1/.github/workflows" "$1/cmake"
    cp -R "$SRC" "$1/ci/images"
    printf 'set(IONIC_KERNEL_REF "%s"\n    CACHE STRING "")\n' \
        "$(sed -n 's/^KERNEL_REF=//p' "$SRC/guest/pins.env")" \
        >"$1/cmake/ErnicKernelModule.cmake"
    cat >"$1/ci/guest-image.env" <<EOF
# The lockfile
$STALE_GUEST

BUILDER_IMAGE=ghcr.io/rocm/rocm-ernic/ci-builder
BUILDER_TAG=stale
EOF
    cat >"$1/.github/workflows/lane.yml" <<'EOF'
jobs:
  a:
    container:
      image: ghcr.io/rocm/rocm-ernic/ci-builder:stale
  b:
    container:
        image:   ghcr.io/rocm/rocm-ernic/ci-builder:older
  c:
    container:
      image: ubuntu:24.04
      # image: ghcr.io/rocm/rocm-ernic/ci-builder:stale
      options: ghcr.io/rocm/rocm-ernic/ci-builder:stale
  d:
    container:
      image: docker.io/other/ci-builder:stale
EOF
    git -C "$1" init -q
    git -C "$1" add -A
    git -C "$1" -c user.name=test -c user.email=test@example.com \
        commit -q -m images
}

REPO="$WORK/repo"
make_repo "$REPO"
IMAGES="$REPO/ci/images"
TAG_SH="$IMAGES/image-tag.sh"

# ── image-tag.sh ────────────────────────────────

TAG=$("$TAG_SH" builder)
[[ $TAG =~ ^qemu[0-9][0-9.]*-vfu\.[0-9a-f]{7}-[0-9a-f]{12}$ ]] ||
    fail "the builder's tag is '$TAG'"
[[ $("$TAG_SH" builder) == "$TAG" ]] || fail "the tag isn't stable"
pass "the tag names the pins and the content: $TAG"

make_repo "$WORK/elsewhere/deeper"
[[ $("$WORK/elsewhere/deeper/ci/images/image-tag.sh" builder) == "$TAG" ]] ||
    fail "the tag depends on where the directory is"
pass "the tag doesn't depend on where the directory is"

[[ $(CI_IMAGES_REGISTRY=ghcr.io/example/proj "$TAG_SH" --ref builder) == \
    "ghcr.io/example/proj/ci-builder:$TAG" ]] ||
    fail "--ref doesn't give REGISTRY/ci-builder:TAG"
[[ $("$TAG_SH" --ref builder) == "ghcr.io/rocm/rocm-ernic/ci-builder:$TAG" ]] ||
    fail "--ref's default registry isn't ghcr.io/rocm/rocm-ernic"
if CI_IMAGES_REGISTRY=ghcr.io/ROCm/x "$TAG_SH" --ref builder \
    >/dev/null 2>&1; then
    fail "an uppercase registry path was accepted"
fi
pass "--ref gives the full reference, in a lowercase registry"

# Check that a change to the tree changes the tag, then undo it.
#   $1  what the change is, for the message
#   $2  a command making it, run in $IMAGES
changes_tag() {
    (cd "$IMAGES" && eval "$2")
    [[ $("$TAG_SH" builder) != "$TAG" ]] || fail "$1 didn't change the tag"
    git -C "$REPO" checkout -q -- .
    git -C "$REPO" clean -q -fd
    [[ $("$TAG_SH" builder) == "$TAG" ]] || fail "undoing $1 left the tag changed"
}

changes_tag "editing the Dockerfile" 'echo "# x" >> builder/Dockerfile'
changes_tag "editing pins.env" 'sed -i "s/^REBUILD=.*/REBUILD=999/" builder/pins.env'
changes_tag "editing image-tag.sh" 'echo "# x" >> image-tag.sh'
changes_tag "editing ensure-image.sh" 'echo "# x" >> ensure-image.sh'
changes_tag "an untracked file" 'echo x > builder/new-file'
changes_tag "making a file executable" 'chmod +x builder/Dockerfile'
changes_tag "removing a file" 'rm builder/check.sh'
pass "every file that defines the image is in its tag"

printf 'builder/ignored\n' >"$IMAGES/.gitignore"
git -C "$REPO" add ci/images/.gitignore
git -C "$REPO" -c user.name=test -c user.email=test@example.com \
    commit -q -m ignore
TAG=$("$TAG_SH" builder)
echo x >"$IMAGES/builder/ignored"
[[ $("$TAG_SH" builder) == "$TAG" ]] || fail "an ignored file changed the tag"
mkdir -p "$IMAGES/other"
echo x >"$IMAGES/other/file"
[[ $("$TAG_SH" builder) == "$TAG" ]] ||
    fail "a file in another image's directory changed the tag"
rm -rf "$IMAGES/builder/ignored" "$IMAGES/other"
pass "ignored files and other images' files aren't in the tag"

# Check that image-tag.sh refuses pins.env with a line in it.
#   $1  what it should say
#   $2  a sed expression editing pins.env
refuses_pins() {
    sed -i "$2" "$IMAGES/builder/pins.env"
    if "$TAG_SH" builder >/dev/null 2>"$WORK/err"; then
        fail "image-tag.sh accepted pins.env edited with '$2'"
    fi
    grep -qF -- "$1" "$WORK/err" ||
        fail "for '$2', image-tag.sh said '$(cat "$WORK/err")', not '$1'"
    git -C "$REPO" checkout -q -- ci/images/builder/pins.env
}

refuses_pins "QEMU_COMMIT must be" 's/^QEMU_COMMIT=.*/QEMU_COMMIT=master/'
refuses_pins "LIBVFIO_USER_COMMIT must be a full SHA" \
    's/^LIBVFIO_USER_COMMIT=.*/LIBVFIO_USER_COMMIT=5b9c722/'
refuses_pins "doesn't set LIBVFIO_USER_COMMIT" '/^LIBVFIO_USER_COMMIT=/d'
refuses_pins "not KEY=VALUE" "s/^UBUNTU=.*/UBUNTU=\$(id)/"
refuses_pins "not KEY=VALUE" 's/^UBUNTU=.*/export UBUNTU=24.04/'
if "$TAG_SH" nosuch >/dev/null 2>&1; then
    fail "image-tag.sh accepted an unknown image"
fi
pass "malformed pins and unknown images are refused"

# ── the lockfile, ci/guest-image.env ────────────

LOCK="$REPO/ci/guest-image.env"
WANT_LOCK="BUILDER_IMAGE=ghcr.io/rocm/rocm-ernic/ci-builder
BUILDER_TAG=$TAG"

if "$TAG_SH" --check builder 2>"$WORK/err"; then
    fail "--check passed a lockfile pinning a stale tag"
fi
grep -qF -- "--write builder" "$WORK/err" ||
    fail "--check didn't say how to fix it: $(cat "$WORK/err")"

# The lockfile names the default registry whatever --ref is told
CI_IMAGES_REGISTRY=ghcr.io/example/proj "$TAG_SH" --write builder 2>/dev/null ||
    fail "--write failed"
[[ $(cat "$LOCK") == "# The lockfile
$STALE_GUEST

$WANT_LOCK" ]] || fail "--write left the lockfile as: $(cat "$LOCK")"
"$TAG_SH" --check builder 2>/dev/null ||
    fail "--check failed right after --write"
CI_IMAGES_REGISTRY=ghcr.io/example/proj "$TAG_SH" --check builder \
    2>/dev/null || fail "--check compared against CI_IMAGES_REGISTRY"

echo "# x" >>"$IMAGES/builder/Dockerfile"
if "$TAG_SH" --check builder 2>/dev/null; then
    fail "--check passed after the builder's definition changed"
fi
git -C "$REPO" checkout -q -- ci/images
sed -i 's|^BUILDER_IMAGE=.*|BUILDER_IMAGE=docker.io/x/ci-builder|' "$LOCK"
if "$TAG_SH" --check builder 2>/dev/null; then
    fail "--check passed a lockfile naming another image"
fi
pass "--write pins the tree's image, and --check fails until it's pinned"

LANE="$REPO/.github/workflows/lane.yml"
[[ $(cat "$LANE") == "jobs:
  a:
    container:
      image: ghcr.io/rocm/rocm-ernic/ci-builder:$TAG
  b:
    container:
        image:   ghcr.io/rocm/rocm-ernic/ci-builder:$TAG
  c:
    container:
      image: ubuntu:24.04
      # image: ghcr.io/rocm/rocm-ernic/ci-builder:stale
      options: ghcr.io/rocm/rocm-ernic/ci-builder:stale
  d:
    container:
      image: docker.io/other/ci-builder:stale" ]] ||
    fail "--write left the workflow as: $(cat "$LANE")"
git -C "$REPO" checkout -q -- ci/guest-image.env
"$TAG_SH" --write builder 2>/dev/null || fail "--write failed"
sed -i "4s|:$TAG\$|:stale|" "$LANE"
if "$TAG_SH" --check builder 2>"$WORK/err"; then
    fail "--check passed a workflow pinning a stale tag"
fi
grep -qF ".github/workflows/lane.yml:4" "$WORK/err" ||
    fail "--check didn't name the stale line: $(cat "$WORK/err")"
grep -qF "ci/guest-image.env" "$WORK/err" &&
    fail "--check named the lockfile, which was right: $(cat "$WORK/err")"
git -C "$REPO" checkout -q -- .github ci/guest-image.env
pass "--write pins the workflows' image: lines, and only those"

# Check that --check and --write refuse a lockfile, and --write leaves it
# alone.
#   $1  what's wrong with it
#   $2  its builder lines
refuses_lock() {
    printf '%s\n' "$2" >"$LOCK"
    for mode in check write; do
        if "$TAG_SH" "--$mode" builder 2>"$WORK/err"; then
            fail "--$mode accepted a lockfile with $1"
        fi
        grep -qF "must set BUILDER_IMAGE and BUILDER_TAG once each" \
            "$WORK/err" || fail "--$mode, $1: $(cat "$WORK/err")"
    done
    [[ $(cat "$LOCK") == "$2" ]] || fail "--write changed a lockfile with $1"
}

refuses_lock "no BUILDER_TAG" "BUILDER_IMAGE=ghcr.io/rocm/rocm-ernic/ci-builder"
refuses_lock "no BUILDER_IMAGE" "BUILDER_TAG=$TAG"
refuses_lock "BUILDER_TAG twice" "$WANT_LOCK
BUILDER_TAG=$TAG"
rm "$LOCK"
if "$TAG_SH" --check builder 2>/dev/null; then
    fail "--check passed with no lockfile"
fi
git -C "$REPO" checkout -q -- ci/guest-image.env
if "$TAG_SH" --check --write builder >/dev/null 2>&1; then
    fail "--check and --write were accepted together"
fi
pass "a lockfile without one pin of each, or none, is refused"

if "$TAG_SH" --check 2>"$WORK/err"; then
    fail "--check with no image passed a lockfile pinning a stale tag"
fi
grep -qF -- "--write builder" "$WORK/err" ||
    fail "--check with no image didn't check the builder: $(cat "$WORK/err")"
"$TAG_SH" --write 2>/dev/null || fail "--write with no image failed"
"$TAG_SH" --check builder 2>/dev/null ||
    fail "--write with no image didn't pin the builder"
"$TAG_SH" --check 2>/dev/null ||
    fail "--check with no image failed right after --write"
git -C "$REPO" checkout -q -- .github ci/guest-image.env

# An image directory image-tag.sh doesn't know fails, and doesn't stop
# the others being checked or written
mkdir "$IMAGES/other"
echo "X=1" >"$IMAGES/other/pins.env"
for mode in check write; do
    if "$TAG_SH" "--$mode" 2>"$WORK/err"; then
        fail "--$mode with no image passed an image it doesn't know"
    fi
    grep -qF "no such image: 'other'" "$WORK/err" ||
        fail "--$mode didn't name the unknown image: $(cat "$WORK/err")"
done
"$TAG_SH" --check builder 2>/dev/null ||
    fail "--write with no image didn't pin the builder past an unknown image"
rm -rf "$IMAGES/other"
git -C "$REPO" checkout -q -- .github ci/guest-image.env

if "$TAG_SH" >/dev/null 2>&1; then
    fail "image-tag.sh with no image printed a tag"
fi
if "$TAG_SH" --ref >/dev/null 2>&1; then
    fail "--ref with no image printed a reference"
fi
pass "--check and --write with no image cover every image directory"

# ── image-tag.sh, the guest ─────────────────────

GTAG=$("$TAG_SH" guest)
KREF=$(sed -n 's/^KERNEL_REF=//p' "$IMAGES/guest/pins.env")
[[ $GTAG =~ ^[a-z]+-k${KREF#v}-[0-9a-f]{12}$ ]] ||
    fail "the guest's tag is '$GTAG'"
[[ $("$TAG_SH" --ref guest) == "ghcr.io/rocm/rocm-ernic/guest-ionic:$GTAG" ]] ||
    fail "--ref guest doesn't give REGISTRY/guest-ionic:TAG"
pass "the guest's tag names its release and kernel: $GTAG"

# Check that a change to the tree changes one image's tag and not the
# other's, then undo it.
#   $1  what the change is, for the message
#   $2  the image whose tag it changes, or "none"
#   $3  a command making it, run in $IMAGES
changes_only() {
    local builder guest
    builder=$("$TAG_SH" builder)
    guest=$("$TAG_SH" guest)
    (cd "$IMAGES" && eval "$3")
    case $2 in
        builder)
            [[ $("$TAG_SH" builder) != "$builder" ]] ||
                fail "$1 didn't change the builder's tag"
            [[ $("$TAG_SH" guest) == "$guest" ]] ||
                fail "$1 changed the guest's tag"
            ;;
        guest)
            [[ $("$TAG_SH" guest) != "$guest" ]] ||
                fail "$1 didn't change the guest's tag"
            [[ $("$TAG_SH" builder) == "$builder" ]] ||
                fail "$1 changed the builder's tag"
            ;;
        none)
            [[ $("$TAG_SH" guest) == "$guest" &&
                $("$TAG_SH" builder) == "$builder" ]] ||
                fail "$1 changed a tag"
            ;;
    esac
    git -C "$REPO" checkout -q -- .
    git -C "$REPO" clean -q -fd
}

changes_only "editing build.sh" guest 'echo "# x" >> guest/build.sh'
changes_only "editing the guest's pins.env" guest \
    'sed -i "s/^REBUILD=.*/REBUILD=999/" guest/pins.env'
changes_only "editing checks/ionic.sh" guest \
    'echo "# x" >> guest/checks/ionic.sh'
changes_only "an untracked file in guest/" guest 'echo x > guest/new-file'
changes_only "editing the Dockerfile" builder \
    'echo "# x" >> builder/Dockerfile'
changes_only "editing ci/boot-guest.sh, outside ci/images/" none \
    'echo x > ../boot-guest.sh'
GTAG=$("$TAG_SH" guest)
(cd "$IMAGES" && echo "# x" >>image-tag.sh)
[[ $("$TAG_SH" guest) != "$GTAG" ]] ||
    fail "editing image-tag.sh didn't change the guest's tag"
git -C "$REPO" checkout -q -- .
pass "each image's tag names its own files, and the scripts all use"

# Check that image-tag.sh refuses the guest's pins.env with a line in it.
#   $1  what it should say
#   $2  a sed expression editing pins.env
refuses_guest_pins() {
    sed -i "$2" "$IMAGES/guest/pins.env"
    if "$TAG_SH" guest >/dev/null 2>"$WORK/err"; then
        fail "image-tag.sh accepted the guest's pins.env edited with '$2'"
    fi
    grep -qF -- "$1" "$WORK/err" ||
        fail "for '$2', image-tag.sh said '$(cat "$WORK/err")', not '$1'"
    git -C "$REPO" checkout -q -- ci/images/guest/pins.env
}

refuses_guest_pins "KERNEL_REF must be" 's/^KERNEL_REF=v/KERNEL_REF=/'
refuses_guest_pins "RELEASE must be" 's/^RELEASE=.*/RELEASE=26.04/'
refuses_guest_pins "doesn't set KERNEL_REF" '/^KERNEL_REF=/d'
pass "the guest's malformed pins are refused"

INFO_TYPE=$(sed -n 's/^INFO_TYPE=//p' "$IMAGES/guest/push.sh")
[[ -n $INFO_TYPE ]] || fail "guest/push.sh sets no INFO_TYPE"
WANT_GUEST="GUEST_ARTIFACT_REPO=ghcr.io/rocm/rocm-ernic/guest-ionic
GUEST_ARTIFACT_TAG=$GTAG
GUEST_ARTIFACT_INFO_TYPE=$INFO_TYPE
GUEST_ARTIFACT_DISK=$(sed -n 's/^VM_NAME=//p' "$IMAGES/guest/pins.env").qcow2
GUEST_ARTIFACT_USER=$(sed -n 's/^USERNAME=//p' "$IMAGES/guest/pins.env")"

if "$TAG_SH" --check guest 2>"$WORK/err"; then
    fail "--check guest passed a lockfile pinning a stale guest"
fi
grep -qF -- "--write guest" "$WORK/err" ||
    fail "--check guest didn't say how to fix it: $(cat "$WORK/err")"
"$TAG_SH" --write guest 2>/dev/null || fail "--write guest failed"
[[ $(cat "$LOCK") == "# The lockfile
$WANT_GUEST

BUILDER_IMAGE=ghcr.io/rocm/rocm-ernic/ci-builder
BUILDER_TAG=stale" ]] || fail "--write guest left the lockfile as: $(cat "$LOCK")"
git -C "$REPO" diff --quiet -- .github ||
    fail "--write guest changed a workflow"
"$TAG_SH" --check guest 2>/dev/null ||
    fail "--check guest failed right after --write guest"
pass "--write guest pins the guest's repository, tag, type, disk and account"

CMAKE="$REPO/cmake/ErnicKernelModule.cmake"
sed -i 's/"v[^"]*"/"v1.2.3"/' "$CMAKE"
if "$TAG_SH" --check guest 2>"$WORK/err"; then
    fail "--check guest passed a guest kernel that isn't IONIC_KERNEL_REF"
fi
grep -qF "IONIC_KERNEL_REF" "$WORK/err" ||
    fail "--check guest didn't name IONIC_KERNEL_REF: $(cat "$WORK/err")"
"$TAG_SH" --write guest 2>/dev/null ||
    fail "--write guest failed over IONIC_KERNEL_REF, which it can't fix"
rm "$CMAKE"
if "$TAG_SH" --check guest 2>/dev/null; then
    fail "--check guest passed with no cmake/ErnicKernelModule.cmake"
fi
git -C "$REPO" checkout -q -- cmake
"$TAG_SH" --check guest 2>/dev/null || fail "--check guest failed once restored"
pass "--check guest fails unless the guest boots IONIC_KERNEL_REF"

sed -i '/^GUEST_ARTIFACT_DISK=/d' "$LOCK"
if "$TAG_SH" --check guest 2>"$WORK/err"; then
    fail "--check guest passed a lockfile with no GUEST_ARTIFACT_DISK"
fi
grep -qF "must set GUEST_ARTIFACT_REPO, GUEST_ARTIFACT_TAG," "$WORK/err" ||
    fail "--check guest: $(cat "$WORK/err")"
git -C "$REPO" checkout -q -- ci/guest-image.env
sed -i '/^INFO_TYPE=/d' "$IMAGES/guest/push.sh"
if "$TAG_SH" --check guest 2>"$WORK/err"; then
    fail "--check guest passed with no INFO_TYPE in push.sh"
fi
grep -qF "sets no INFO_TYPE" "$WORK/err" ||
    fail "--check guest: $(cat "$WORK/err")"
git -C "$REPO" checkout -q -- ci/images
pass "a lockfile without each guest pin, or a push.sh without its type, fails"

# ── ensure-image.sh ─────────────────────────────

# docker: records each call, one argument to a line. FAKE_INSPECT says
# what the registry answers (published, missing, missing-repo, denied),
# and FAKE_INSPECT_AGAIN, if set, what it answers when asked again;
# FAKE_CHECK_FAIL makes the image's check fail; FAKE_PODMAN makes it
# podman-docker's docker.
BIN="$WORK/bin"
mkdir -p "$BIN"
cat >"$BIN/docker" <<'EOF'
#!/usr/bin/env bash
{ printf '%s\n' "$@"; echo --; } >>"$FAKE_DOCKER_LOG"
case "$1 ${2:-}" in
    "--version ")
        if [[ -n ${FAKE_PODMAN:-} ]]; then
            echo "podman version 5.7.0"
        else
            echo "Docker version 28.5.0, build 1234567"
        fi
        ;;
    "buildx version") echo "github.com/docker/buildx v0.29.0" ;;
    "buildx imagetools")
        answer=$FAKE_INSPECT
        if [[ -n ${FAKE_INSPECT_AGAIN:-} &&
            $(grep -cx imagetools "$FAKE_DOCKER_LOG") -gt 1 ]]; then
            answer=$FAKE_INSPECT_AGAIN
        fi
        case $answer in
            published) echo "Name: $4" ;;
            missing)
                echo "ERROR: $4: not found" >&2
                exit 1
                ;;
            missing-repo)
                echo "ERROR: NAME_UNKNOWN: repository name not known" >&2
                exit 1
                ;;
            denied)
                echo "ERROR: denied: permission_denied" >&2
                exit 1
                ;;
        esac
        ;;
    "buildx build") ;;
    "run "*)
        if [[ -n ${FAKE_CHECK_FAIL:-} ]]; then
            exit 1
        fi
        ;;
    "push "*) ;;
    *)
        echo "unexpected docker call: $*" >&2
        exit 99
        ;;
esac
EOF
cat >"$BIN/podman" <<'EOF'
#!/usr/bin/env bash
{ echo podman; printf '%s\n' "$@"; echo --; } >>"$FAKE_DOCKER_LOG"
EOF
chmod +x "$BIN/docker" "$BIN/podman"

ENSURE="$IMAGES/ensure-image.sh"
export CI_IMAGES_REGISTRY=ghcr.io/example/proj
REF=$("$TAG_SH" --ref builder)

# Run ensure-image.sh with the stand-ins, its output in $WORK/out and
# $WORK/err, and docker's calls in $WORK/docker.log.
#   $@  environment as NAME=VALUE, then -- and its arguments
ensure() {
    local env=()
    while [[ $1 != -- ]]; do
        env+=("$1")
        shift
    done
    shift
    : >"$WORK/docker.log"
    env PATH="$BIN:$PATH" FAKE_DOCKER_LOG="$WORK/docker.log" "${env[@]}" \
        "$ENSURE" "$@" >"$WORK/out" 2>"$WORK/err"
}

# Check whether docker was called with an argument.
#   $1  the argument
called_with() {
    grep -qxF -- "$1" "$WORK/docker.log"
}

ensure FAKE_INSPECT=published -- builder ||
    fail "a published image failed: $(cat "$WORK/err")"
[[ $(cat "$WORK/out") == "$REF" ]] || fail "it didn't print $REF"
! called_with build || fail "a published image was built again"
! called_with push || fail "a published image was pushed again"
pass "a published image is left alone"

for answer in missing missing-repo; do
    ensure FAKE_INSPECT=$answer -- builder ||
        fail "$answer: building failed: $(cat "$WORK/err")"
    [[ $(cat "$WORK/out") == "$REF" ]] || fail "$answer: it didn't print $REF"
    called_with build || fail "$answer: a missing image wasn't built"
    called_with "type=registry,ref=${REF%:*}:buildcache,mode=max" ||
        fail "$answer: the build didn't export its cache"
    called_with "LIBVFIO_USER_COMMIT=$(sed -n 's/^LIBVFIO_USER_COMMIT=//p' \
        "$IMAGES/builder/pins.env")" ||
        fail "$answer: the pins weren't passed as build arguments"
    called_with ROOT_CA_SHA256=none ||
        fail "$answer: the build wasn't told it has no root CA"
    called_with "$IMAGES/builder/pins.env" ||
        fail "$answer: the check wasn't given the pins"
    called_with "$IMAGES/builder/check.sh:/check.sh:ro" ||
        fail "$answer: the image's check wasn't run"
    called_with push || fail "$answer: the image wasn't pushed"
    # The check, then another look in the registry, come before the push
    run_at=$(grep -nx run "$WORK/docker.log" | cut -d: -f1)
    looked_at=$(grep -nx imagetools "$WORK/docker.log" | tail -1 | cut -d: -f1)
    push_at=$(grep -nx push "$WORK/docker.log" | cut -d: -f1)
    [[ $run_at -lt $push_at ]] ||
        fail "$answer: the image was pushed before it was checked"
    [[ $run_at -lt $looked_at && $looked_at -lt $push_at ]] ||
        fail "$answer: it didn't look in the registry again before pushing"
done
pass "a missing image is built, checked, then pushed"

ensure FAKE_INSPECT=missing FAKE_INSPECT_AGAIN=published -- builder ||
    fail "an image published during the build failed: $(cat "$WORK/err")"
[[ $(cat "$WORK/out") == "$REF" ]] || fail "it didn't print $REF"
called_with build || fail "the missing image wasn't built"
! called_with push || fail "an image published during the build was pushed"
if ensure FAKE_INSPECT=missing FAKE_INSPECT_AGAIN=denied -- builder; then
    fail "an unclear answer after the build was taken as missing"
fi
! called_with push || fail "an image was pushed on an unclear answer"
pass "an image published while this run built it isn't pushed again"

if ensure FAKE_INSPECT=denied -- builder; then
    fail "an unclear answer from the registry was taken as missing"
fi
grep -q "couldn't tell whether" "$WORK/err" ||
    fail "the unclear answer wasn't reported: $(cat "$WORK/err")"
! called_with build || fail "an image was built on an unclear answer"
pass "an unclear answer from the registry builds nothing"

if ensure FAKE_INSPECT=missing FAKE_CHECK_FAIL=1 -- builder; then
    fail "an image that failed its check succeeded"
fi
! called_with push || fail "an image that failed its check was pushed"
pass "an image that fails its check isn't pushed"

mv "$IMAGES/builder/check.sh" "$WORK/check.sh"
for mode in publish build-only; do
    args=(builder)
    if [[ $mode == build-only ]]; then
        args=(--build-only builder)
    fi
    if ensure FAKE_INSPECT=missing -- "${args[@]}"; then
        fail "$mode: an image with no check.sh succeeded"
    fi
    grep -q "check.sh is missing" "$WORK/err" ||
        fail "$mode: it didn't say check.sh is missing: $(cat "$WORK/err")"
    ! called_with build || fail "$mode: an image with no check.sh was built"
done
mv "$WORK/check.sh" "$IMAGES/builder/check.sh"
pass "an image with no check.sh isn't built, let alone published"

if ensure FAKE_INSPECT=missing CI_IMAGES_CAN_PUSH=false -- builder; then
    fail "a run that can't push succeeded with the image missing"
fi
grep -q "from a fork can't push images" "$WORK/err" ||
    fail "it didn't say why: $(cat "$WORK/err")"
! called_with build || fail "a run that can't push built the image"
ensure FAKE_INSPECT=published CI_IMAGES_CAN_PUSH=false -- builder ||
    fail "a run that can't push failed though the image is published"
pass "a run that can't push fails only when the image is missing"

ensure FAKE_INSPECT=denied -- --build-only builder ||
    fail "--build-only failed: $(cat "$WORK/err")"
! called_with imagetools || fail "--build-only looked in the registry"
called_with build || fail "--build-only didn't build"
! grep -q 'type=registry' "$WORK/docker.log" ||
    fail "--build-only imported or exported a registry cache"
called_with ROOT_CA_SHA256=none ||
    fail "a build without a root CA wasn't told ROOT_CA_SHA256=none"
called_with run || fail "--build-only didn't check the image"
! called_with push || fail "--build-only pushed"
pass "--build-only builds and checks, and touches no registry"

echo cert >"$WORK/root-ca.crt"
ensure CI_IMAGES_ROOT_CA="$WORK/root-ca.crt" -- --build-only builder ||
    fail "--build-only with a root CA failed: $(cat "$WORK/err")"
called_with "id=root-ca,src=$WORK/root-ca.crt" ||
    fail "the root CA wasn't passed to the build as a secret"
ca_sum=$(sha256sum <"$WORK/root-ca.crt")
called_with "ROOT_CA_SHA256=${ca_sum%% *}" ||
    fail "the root CA's SHA-256 wasn't passed, to key the build cache"
! called_with ROOT_CA_SHA256=none ||
    fail "a build with a root CA was told ROOT_CA_SHA256=none"
if ensure CI_IMAGES_ROOT_CA="$WORK/root-ca.crt" FAKE_INSPECT=missing \
    -- builder; then
    fail "an image built trusting a local root CA could be published"
fi
! called_with build || fail "a build to publish was given a local root CA"
pass "a local root CA, and its SHA-256, are passed to local builds only"

ensure FAKE_PODMAN=1 -- --build-only builder ||
    fail "--build-only with podman failed: $(cat "$WORK/err")"
grep -qx podman "$WORK/docker.log" || fail "--build-only didn't use podman"
if ensure FAKE_PODMAN=1 FAKE_INSPECT=missing -- builder; then
    fail "publishing went ahead without docker buildx"
fi
grep -q "docker with buildx not found" "$WORK/err" ||
    fail "it didn't say docker is missing: $(cat "$WORK/err")"
pass "podman-docker's docker is podman: only --build-only uses it"

# ── guest/push.sh ───────────────────────────────

# oras: records each call, one argument to a line. FAKE_ORAS says what the
# registry answers a manifest fetch (published, missing, missing-repo,
# denied), and FAKE_ORAS_AGAIN, if set, what it answers when asked again;
# FAKE_ORAS_FAIL names a command (push, attach, tag) that fails.
DIGEST=sha256:$(printf 'a%.0s' {1..64})
cat >"$BIN/oras" <<EOF
#!/usr/bin/env bash
{ echo oras; printf '%s\n' "\$@"; echo --; } >>"\$FAKE_DOCKER_LOG"
if [[ \${FAKE_ORAS_FAIL:-} == "\$1" ]]; then
    echo "Error: \$1 failed" >&2
    exit 1
fi
case \$1 in
    manifest)
        answer=\$FAKE_ORAS
        if [[ -n \${FAKE_ORAS_AGAIN:-} &&
            \$(grep -cx manifest "\$FAKE_DOCKER_LOG") -gt 1 ]]; then
            answer=\$FAKE_ORAS_AGAIN
        fi
        case \$answer in
            published) echo '{"digest":"$DIGEST"}' ;;
            missing)
                echo "Error: \$4: not found" >&2
                exit 1
                ;;
            missing-repo)
                echo "Error: NAME_UNKNOWN: repository name not known" >&2
                exit 1
                ;;
            denied)
                echo "Error: denied: permission_denied" >&2
                exit 1
                ;;
        esac
        ;;
    push) echo "$DIGEST" ;;
    attach | tag) ;;
    *)
        echo "unexpected oras call: \$*" >&2
        exit 99
        ;;
esac
EOF
# zstd: records its arguments, and "compresses" by copying
cat >"$BIN/zstd" <<'EOF'
#!/usr/bin/env bash
{ echo zstd; printf '%s\n' "$@"; echo --; } >>"$FAKE_DOCKER_LOG"
out="" in=""
while [[ $# -gt 0 ]]; do
    case $1 in
        -o) out=$2; shift 2 ;;
        --) in=$2; shift 2 ;;
        *) shift ;;
    esac
done
cp -- "$in" "$out"
EOF
chmod +x "$BIN/oras" "$BIN/zstd"

VM_NAME=$(sed -n 's/^VM_NAME=//p' "$IMAGES/guest/pins.env")
GOUT="$WORK/guest-out"
mkdir -p "$GOUT"
echo disk >"$GOUT/$VM_NAME.qcow2"
jq -n --arg image "$VM_NAME.qcow2" '{
    image_path: $image, release: "resolute", flavour: "ionic",
    username: "ubuntu", kernel_release: "7.2.6-070206-generic",
    build_info: {build_timestamp: "2026-11-12T00:00:00Z",
                 source_revision: "0123abc"}
}' >"$GOUT/vm-info.json"
PUSH="$IMAGES/guest/push.sh"
PREF=ghcr.io/example/proj/guest-ionic

# Run push.sh with the stand-ins, its output in $WORK/out and $WORK/err,
# and their calls in $WORK/docker.log.
#   $@  environment as NAME=VALUE, then -- and its arguments
push() {
    local env=()
    while [[ $1 != -- ]]; do
        env+=("$1")
        shift
    done
    shift
    : >"$WORK/docker.log"
    env PATH="$BIN:$PATH" FAKE_DOCKER_LOG="$WORK/docker.log" \
        FAKE_ORAS=missing VM_NAME="$VM_NAME" "${env[@]}" "$PUSH" "$@" \
        >"$WORK/out" 2>"$WORK/err"
}

# The line number of an oras command's call in the log
#   $1  the command
oras_at() {
    grep -nxA1 oras "$WORK/docker.log" | sed -n "s/^\([0-9]*\)-$1\$/\1/p" |
        head -n 1
}

push -- "$GOUT" "$PREF:t1" || fail "push.sh failed: $(cat "$WORK/err")"
called_with "$PREF" || fail "the disk wasn't pushed untagged"
called_with "application/vnd.rocm-ernic.vm-image.v1" ||
    fail "the disk wasn't pushed as a rocm-ernic VM image"
called_with "$VM_NAME.qcow2.zst:application/vnd.rocm-ernic.vm-image.layer.v1+zstd" ||
    fail "the compressed disk wasn't the layer"
called_with "--long=27" || fail "the disk wasn't compressed with --long=27"
called_with "vm.kernel_release=7.2.6-070206-generic" ||
    fail "the kernel wasn't an annotation"
called_with "$PREF@$DIGEST" || fail "nothing was done to the pushed digest"
called_with "$INFO_TYPE" || fail "vm-info.json wasn't attached as $INFO_TYPE"
called_with "vm-info.json:application/json" ||
    fail "vm-info.json wasn't attached"
called_with t1 || fail "the digest wasn't tagged"
pushed_at=$(oras_at push)
attached_at=$(oras_at attach)
tagged_at=$(oras_at tag)
[[ -n $pushed_at && $pushed_at -lt $attached_at &&
    $attached_at -lt $tagged_at ]] ||
    fail "not pushed, then attached, then tagged: $pushed_at $attached_at $tagged_at"
[[ $(ls -A "$GOUT") == $'ubuntu-ci-vm.qcow2\nvm-info.json' ]] ||
    fail "push.sh left files behind: $(ls -A "$GOUT")"
looked_at=$(oras_at manifest)
[[ -n $looked_at && $attached_at -lt $looked_at &&
    $looked_at -lt $tagged_at ]] ||
    fail "it didn't look for the tag again just before tagging"
pass "push.sh pushes the disk by digest, attaches vm-info.json, then tags it"

push FAKE_ORAS=published -- "$GOUT" "$PREF:t1" ||
    fail "push.sh failed when the tag appeared meanwhile: $(cat "$WORK/err")"
[[ -z $(oras_at tag) ]] || fail "a tag that appeared meanwhile was moved"
if push FAKE_ORAS=denied -- "$GOUT" "$PREF:t1"; then
    fail "push.sh tagged on an unclear answer from the registry"
fi
[[ -z $(oras_at tag) ]] || fail "it tagged on an unclear answer"
pass "push.sh leaves a tag that appeared while it pushed where it is"

for step in push attach; do
    if push FAKE_ORAS_FAIL=$step -- "$GOUT" "$PREF:t1"; then
        fail "push.sh succeeded though oras $step failed"
    fi
    [[ -z $(oras_at tag) ]] || fail "a failed $step was tagged"
done
pass "a push that fails part way leaves no tag"

jq '.image_path = "other.qcow2"' "$GOUT/vm-info.json" >"$WORK/info"
cp "$WORK/info" "$GOUT/vm-info.json"
if push -- "$GOUT" "$PREF:t1"; then
    fail "push.sh pushed a disk vm-info.json doesn't describe"
fi
[[ -z $(oras_at push) ]] || fail "a disk vm-info.json doesn't describe was pushed"
jq '.image_path = "ubuntu-ci-vm.qcow2"' "$WORK/info" >"$GOUT/vm-info.json"
if push -- "$GOUT" "$PREF"; then
    fail "push.sh took a reference with no tag"
fi
pass "push.sh refuses a disk vm-info.json doesn't describe, or no tag"

# ── ensure-image.sh, the guest ──────────────────

# Stand-ins for the guest's scripts, recording their arguments and the
# pins and settings they're given. FAKE_CHECK_FAIL fails the check.
for script in build check push; do
    cat >"$IMAGES/guest/$script.sh" <<EOF
#!/usr/bin/env bash
{
    echo $script.sh
    printf '%s\n' "\$@"
    echo "VM_NAME=\${VM_NAME:-}"
    echo "ROOT_CA=\${CI_IMAGES_ROOT_CA:-}"
    echo --
} >>"\$FAKE_DOCKER_LOG"
EOF
done
cat >>"$IMAGES/guest/build.sh" <<'EOF'
if [[ -e $1 ]]; then
    printf '%s\n' "out exists" -- >>"$FAKE_DOCKER_LOG"
fi
mkdir -p "$1"
EOF
cat >>"$IMAGES/guest/check.sh" <<'EOF'
[[ -z ${FAKE_CHECK_FAIL:-} ]]
EOF
chmod +x "$IMAGES/guest/"{build,check,push}.sh
GREF=$("$TAG_SH" --ref guest)
GOUT_DEFAULT="$REPO/build-ci-images/guest"

ensure FAKE_ORAS=published -- guest ||
    fail "a published guest failed: $(cat "$WORK/err")"
[[ $(cat "$WORK/out") == "$GREF" ]] || fail "it didn't print $GREF"
called_with "$GREF" || fail "it didn't look for $GREF"
! called_with build.sh || fail "a published guest was built again"
! called_with push.sh || fail "a published guest was pushed again"
pass "a published guest is left alone"

for answer in missing missing-repo; do
    mkdir -p "$GOUT_DEFAULT"
    echo stale >"$GOUT_DEFAULT/stale"
    ensure FAKE_ORAS=$answer -- guest ||
        fail "$answer: building the guest failed: $(cat "$WORK/err")"
    [[ $(cat "$WORK/out") == "$GREF" ]] || fail "$answer: it didn't print $GREF"
    called_with build.sh || fail "$answer: a missing guest wasn't built"
    ! called_with "out exists" ||
        fail "$answer: the build's directory wasn't emptied first"
    called_with "$GOUT_DEFAULT" ||
        fail "$answer: it wasn't built in build-ci-images/guest"
    called_with "VM_NAME=$VM_NAME" ||
        fail "$answer: the pins weren't in the scripts' environment"
    built_at=$(grep -nx build.sh "$WORK/docker.log" | cut -d: -f1)
    checked_at=$(grep -nx check.sh "$WORK/docker.log" | cut -d: -f1)
    looked_at=$(grep -nx manifest "$WORK/docker.log" | tail -1 | cut -d: -f1)
    pushed_at=$(grep -nx push.sh "$WORK/docker.log" | cut -d: -f1)
    [[ -n $pushed_at && $built_at -lt $checked_at &&
        $checked_at -lt $looked_at && $looked_at -lt $pushed_at ]] ||
        fail "$answer: not built, checked, looked for again, then pushed"
    called_with "$GREF" || fail "$answer: push.sh wasn't given $GREF"
done
pass "a missing guest is built, checked, then pushed"

ensure FAKE_ORAS=missing FAKE_ORAS_AGAIN=published -- guest ||
    fail "a guest published during the build failed: $(cat "$WORK/err")"
! called_with push.sh || fail "a guest published during the build was pushed"
if ensure FAKE_ORAS=denied -- guest; then
    fail "an unclear answer from the registry was taken as missing"
fi
! called_with build.sh || fail "a guest was built on an unclear answer"
if ensure FAKE_ORAS=missing FAKE_CHECK_FAIL=1 -- guest; then
    fail "a guest that failed its check succeeded"
fi
! called_with push.sh || fail "a guest that failed its check was pushed"
if ensure FAKE_ORAS=missing CI_IMAGES_CAN_PUSH=false -- guest; then
    fail "a run that can't push succeeded with the guest missing"
fi
! called_with build.sh || fail "a run that can't push built the guest"
pass "a guest is pushed only if it's missing, passes its check, and can be"

ensure FAKE_ORAS=denied CI_IMAGES_ROOT_CA="$WORK/root-ca.crt" \
    -- --build-only guest ||
    fail "--build-only guest failed: $(cat "$WORK/err")"
! called_with oras || fail "--build-only guest used oras"
called_with build.sh || fail "--build-only guest didn't build"
called_with check.sh || fail "--build-only guest didn't check"
! called_with push.sh || fail "--build-only guest pushed"
called_with "ROOT_CA=$WORK/root-ca.crt" ||
    fail "the root CA wasn't passed to build.sh"
mkdir -p "$WORK/my-out"
echo keep >"$WORK/my-out/keep"
ensure CI_IMAGES_OUT="$WORK/my-out" -- --build-only guest ||
    fail "--build-only guest into CI_IMAGES_OUT failed: $(cat "$WORK/err")"
called_with "$WORK/my-out" || fail "the guest wasn't built in CI_IMAGES_OUT"
[[ -f $WORK/my-out/keep ]] || fail "CI_IMAGES_OUT was emptied"
pass "--build-only builds and checks the guest, and touches no registry"

rm "$IMAGES/guest/build.sh"
if ensure FAKE_ORAS=missing -- guest; then
    fail "an image with neither a Dockerfile nor build.sh was built"
fi
grep -qF "neither a Dockerfile nor a build.sh and push.sh" "$WORK/err" ||
    fail "it didn't say what's missing: $(cat "$WORK/err")"
pass "an image with no Dockerfile or build.sh isn't built"

echo "All CI image tests passed."
