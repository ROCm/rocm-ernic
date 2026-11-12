#!/usr/bin/env bash
#
# ci/images/image-tag.sh and ci/images/ensure-image.sh, on a copy of
# ci/images/ in a scratch git repository: that the tag names exactly the
# files that define an image, wherever the directory is, and that
# ensure-image.sh builds and pushes only an image the registry says it
# hasn't got, and never one that fails its checks. A stand-in for docker
# records what it's asked to do; nothing is built.
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

command -v git >/dev/null || fail "git is needed and not installed"

# A git repository holding a copy of ci/images/, all committed
#   $1  where
make_repo() {
    mkdir -p "$1/ci"
    cp -R "$SRC" "$1/ci/images"
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
[[ $("$TAG_SH" builder) == "$("$SRC/image-tag.sh" builder)" ]] ||
    fail "the copy's tag isn't the source tree's"
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

# ── ensure-image.sh ─────────────────────────────

# docker: records each call, one argument to a line. FAKE_INSPECT says
# what the registry answers (published, missing, missing-repo, denied);
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
        case $FAKE_INSPECT in
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
    called_with "$IMAGES/builder/pins.env" ||
        fail "$answer: the check wasn't given the pins"
    called_with "$IMAGES/builder/check.sh:/check.sh:ro" ||
        fail "$answer: the image's check wasn't run"
    called_with push || fail "$answer: the image wasn't pushed"
    # The check comes before the push
    [[ $(grep -nx run "$WORK/docker.log" | cut -d: -f1) -lt \
        $(grep -nx push "$WORK/docker.log" | cut -d: -f1) ]] ||
        fail "$answer: the image was pushed before it was checked"
done
pass "a missing image is built, checked, then pushed"

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
! called_with "type=registry,ref=${REF%:*}:buildcache,mode=max" ||
    fail "--build-only exported a cache"
called_with run || fail "--build-only didn't check the image"
! called_with push || fail "--build-only pushed"
pass "--build-only builds and checks, and touches no registry"

echo cert >"$WORK/root-ca.crt"
ensure CI_IMAGES_ROOT_CA="$WORK/root-ca.crt" -- --build-only builder ||
    fail "--build-only with a root CA failed: $(cat "$WORK/err")"
called_with "id=root-ca,src=$WORK/root-ca.crt" ||
    fail "the root CA wasn't passed to the build as a secret"
if ensure CI_IMAGES_ROOT_CA="$WORK/root-ca.crt" FAKE_INSPECT=missing \
    -- builder; then
    fail "an image built trusting a local root CA could be published"
fi
! called_with build || fail "a build to publish was given a local root CA"
pass "a local root CA is passed to local builds only"

ensure FAKE_PODMAN=1 -- --build-only builder ||
    fail "--build-only with podman failed: $(cat "$WORK/err")"
grep -qx podman "$WORK/docker.log" || fail "--build-only didn't use podman"
if ensure FAKE_PODMAN=1 FAKE_INSPECT=missing -- builder; then
    fail "publishing went ahead without docker buildx"
fi
grep -q "docker with buildx not found" "$WORK/err" ||
    fail "it didn't say docker is missing: $(cat "$WORK/err")"
pass "podman-docker's docker is podman: only --build-only uses it"

echo "All CI image tests passed."
