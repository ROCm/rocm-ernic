#!/usr/bin/env bash
#
# ci/images/prune-images.sh, against stand-ins for gh and for GHCR's
# registry (curl): which versions of an image it keeps and deletes, that
# it keeps whatever a kept version needs, and that it deletes nothing
# when anything goes wrong first. Nothing is deleted from GHCR.
#
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Usage: test_prune_images.sh PRUNE_IMAGES_SH
#
# Exit 0 = pass, exit 1 = fail.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 PRUNE_IMAGES_SH" >&2
    exit 1
fi
PRUNE=$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")

WORK=$(mktemp -d "${TMPDIR:-/tmp}/test-prune-images-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
trap 'exit 1' HUP INT TERM

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

pass() {
    echo "PASS: $*"
}

command -v jq >/dev/null || fail "jq is needed and not installed"

# The stand-ins' data: GH/<package>.json is what `gh api --paginate`
# prints for a package's versions, and REG/<hex> each manifest
GH="$WORK/gh"
REG="$WORK/registry"
BIN="$WORK/bin"
mkdir -p "$GH" "$REG" "$BIN"

# gh: lists a package's versions from $GH, or says it's not found, and
# records each deletion in $FAKE_LOG. FAKE_GH_FAIL makes listing fail;
# FAKE_DELETE_FAIL makes deleting fail.
cat >"$BIN/gh" <<'EOF'
#!/usr/bin/env bash
method=GET
url=""
while [[ $# -gt 0 ]]; do
    case $1 in
        api | --paginate | --silent) ;;
        --method)
            method=$2
            shift
            ;;
        *) url=$1 ;;
    esac
    shift
done
if [[ $method == DELETE ]]; then
    if [[ -n ${FAKE_DELETE_FAIL:-} ]]; then
        echo "gh: Forbidden (HTTP 403)" >&2
        exit 1
    fi
    echo "DELETE $url" >>"$FAKE_LOG"
    exit 0
fi
if [[ -n ${FAKE_GH_FAIL:-} ]]; then
    echo "gh: Server Error (HTTP 500)" >&2
    exit 1
fi
[[ $url =~ ^/orgs/rocm/packages/container/rocm-ernic%2F([^/]+)/versions\?per_page=100$ ]] ||
    { echo "unexpected gh call: $url" >&2; exit 99; }
if [[ ! -f $FAKE_GH/${BASH_REMATCH[1]}.json ]]; then
    echo "gh: Not Found (HTTP 404)" >&2
    exit 1
fi
cat "$FAKE_GH/${BASH_REMATCH[1]}.json"
EOF

# curl: hands out a token, and serves the manifests in $FAKE_REG.
# FAKE_CURL_FAIL makes reading manifests fail; FAKE_CURL_WRONG serves
# another manifest than the one asked for.
cat >"$BIN/curl" <<'EOF'
#!/usr/bin/env bash
out=""
url=""
while [[ $# -gt 0 ]]; do
    case $1 in
        -o)
            out=$2
            shift
            ;;
        -H)
            shift
            ;;
        -*) ;;
        *) url=$1 ;;
    esac
    shift
done
case $url in
    https://ghcr.io/token\?scope=repository:rocm/rocm-ernic/*:pull)
        echo '{"token":"anonymous"}'
        ;;
    https://ghcr.io/v2/rocm/rocm-ernic/*/manifests/sha256:*)
        hex=${url##*sha256:}
        [[ -z ${FAKE_CURL_FAIL:-} && -f $FAKE_REG/$hex ]] || exit 22
        if [[ -n ${FAKE_CURL_WRONG:-} ]]; then
            echo '{}' >"$out"
        else
            cp "$FAKE_REG/$hex" "$out"
        fi
        ;;
    *)
        echo "unexpected curl call: $url" >&2
        exit 99
        ;;
esac
EOF
chmod +x "$BIN/gh" "$BIN/curl"

NOW=$(date -u +%s)
NEXT_ID=1000

# A content tag like image-tag.sh's, from a number
#   $1  the number
ctag() {
    printf 'img-%012x\n' "$1"
}

# Store a manifest in the registry and print its digest. Each is made
# unique by NAME.
#   $1  NAME
#   $2  extra JSON fields, or empty
manifest() {
    local json sum
    json="{\"schemaVersion\":2,\"annotations\":{\"name\":\"$1\"}${2:+,$2}}"
    printf '%s' "$json" >"$WORK/m"
    sum=$(sha256sum <"$WORK/m")
    mv "$WORK/m" "$REG/${sum%% *}"
    echo "sha256:${sum%% *}"
}

# An index listing manifests
#   $1  NAME
#   $@  the manifests' digests
index() {
    local name=$1 list="" d
    shift
    for d in "$@"; do
        list+="${list:+,}{\"digest\":\"$d\"}"
    done
    manifest "$name" "\"manifests\":[$list]"
}

# Add a version to a package, one page of `gh api` per call, and record
# its ID as ID[NAME].
#   $1  the package (ci-builder)
#   $2  NAME, for ID[]
#   $3  its digest
#   $4  its age in days
#   $@  its tags
declare -A ID=()
version() {
    local package=$1 name=$2 digest=$3 days=$4 created
    shift 4
    created=$(date -u -d "@$((NOW - days * 86400 - 60))" +%Y-%m-%dT%H:%M:%SZ)
    NEXT_ID=$((NEXT_ID + 1))
    ID[$name]=$NEXT_ID
    jq -cn --argjson id "$NEXT_ID" --arg d "$digest" --arg c "$created" \
        '[{id: $id, name: $d, created_at: $c,
           metadata: {container: {tags: $ARGS.positional}}}]' \
        --args "$@" >>"$GH/$package.json"
}

LOCK="$WORK/guest-image.env"
OUT="$WORK/out"
ERR="$WORK/err"
LOG="$WORK/log"

# Run prune-images.sh with the stand-ins, its output in $OUT and $ERR,
# and the deletions in $LOG.
#   $@  environment as NAME=VALUE, then -- and its arguments
prune() {
    local env=()
    while [[ $1 != -- ]]; do
        env+=("$1")
        shift
    done
    shift
    : >"$LOG"
    env PATH="$BIN:$PATH" FAKE_GH="$GH" FAKE_REG="$REG" FAKE_LOG="$LOG" \
        "${env[@]}" "$PRUNE" "$@" >"$OUT" 2>"$ERR"
}

# The IDs deleted, sorted
deleted() {
    sed -n 's|^DELETE .*/versions/||p' "$LOG" | sort -n | tr '\n' ' '
}

# The IDs of versions, sorted
#   $@  their NAMEs
ids_of() {
    local name
    for name in "$@"; do
        echo "${ID[$name]}"
    done | sort -n | tr '\n' ' '
}

# ── the builder: tags, the cache, and untagged leftovers ──

# Content tags 1-14, made a week apart up to 12 days ago, so 1-3 are
# under 30 days old and 1-10 the newest. 13 is pinned.
for i in $(seq 1 14); do
    version ci-builder "tag$i" "$(manifest "tag$i")" $((i * 7 + 5)) \
        "$(ctag "$i")"
done
version ci-builder cache "$(manifest cache)" 90 buildcache
version ci-builder oldcache1 "$(manifest oldcache1)" 60
version ci-builder oldcache2 "$(manifest oldcache2)" 31
version ci-builder newcache "$(manifest newcache)" 2

# A pinned index whose manifests are old and untagged, one shared with
# an unpinned old index, and that index's own manifest
declare -A DIGEST=()
for name in kept-amd64 shared deleted-amd64; do
    DIGEST[$name]=$(manifest "$name")
    version ci-builder "$name" "${DIGEST[$name]}" 200
done
version ci-builder kept-index \
    "$(index kept-index "${DIGEST[kept-amd64]}" "${DIGEST[shared]}")" 200 \
    "$(ctag 200)"
version ci-builder deleted-index \
    "$(index deleted-index "${DIGEST[deleted-amd64]}" "${DIGEST[shared]}")" \
    200 "$(ctag 201)"

cat >"$LOCK" <<EOF
# A lockfile
BUILDER_IMAGE=ghcr.io/rocm/rocm-ernic/ci-builder
BUILDER_TAG=$(ctag 13)
GUEST_ARTIFACT_REPO=docker.io/someone/guest
GUEST_ARTIFACT_TAG=elsewhere
EOF
# The default branch's, pinning the index
cat >"$WORK/develop.env" <<EOF
BUILDER_IMAGE=ghcr.io/rocm/rocm-ernic/ci-builder
BUILDER_TAG=$(ctag 200)
EOF

WANT=$(ids_of tag11 tag12 tag14 oldcache1 oldcache2 deleted-index \
    deleted-amd64)

prune -- "$LOCK" "$WORK/develop.env" ||
    fail "a dry run failed: $(cat "$ERR")"
[[ ! -s $LOG ]] || fail "a dry run deleted: $(cat "$LOG")"
grep -q "23 versions: keeping 16, deleting 7" "$OUT" ||
    fail "a dry run didn't count what it would delete: $(cat "$OUT")"
grep -q "^Nothing deleted without --delete$" "$OUT" ||
    fail "a dry run didn't say it deleted nothing: $(cat "$OUT")"
pass "without --delete, nothing is deleted"

prune -- --delete "$LOCK" "$WORK/develop.env" ||
    fail "--delete failed: $(cat "$ERR")"
[[ $(deleted) == "$WANT" ]] ||
    fail "deleted $(deleted), not $WANT: $(cat "$OUT")"
grep -q "^Deleted 7 versions$" "$OUT" ||
    fail "it didn't say it deleted 7: $(cat "$OUT")"
pass "old tags, old untagged versions and what only they need are deleted"

for n in 13 200; do
    grep -q "^  keep .* $(ctag "$n")  (pinned)$" "$OUT" ||
        fail "$(ctag "$n") wasn't kept as pinned: $(cat "$OUT")"
done
grep -q "^  keep .* buildcache  (tagged buildcache)$" "$OUT" ||
    fail "the cache wasn't kept for its tag: $(cat "$OUT")"
[[ $(grep -c "(under 30 days old)$" "$OUT") -eq 4 ]] ||
    fail "the 4 versions under 30 days old weren't kept: $(cat "$OUT")"
[[ $(grep -c "(among the 10 newest)$" "$OUT") -eq 7 ]] ||
    fail "the rest of the 10 newest weren't kept: $(cat "$OUT")"
for name in kept-amd64 shared; do
    grep -q "^  keep .* ${DIGEST[$name]:0:19}  (untagged)  (needed by " \
        "$OUT" || fail "the pinned index's $name wasn't kept: $(cat "$OUT")"
done
pass "a version is kept for its pin, other tags, age, rank or a kept index"

# Deletions go tagged first, so an untagged manifest goes only after the
# index listing it
last_tagged=$(grep -n "/versions/${ID[deleted-index]}\$" "$LOG" | cut -d: -f1)
first_untagged=$(grep -n "/versions/${ID[deleted-amd64]}\$" "$LOG" |
    cut -d: -f1)
[[ $last_tagged -lt $first_untagged ]] ||
    fail "a manifest was deleted before the index listing it: $(cat "$LOG")"
pass "tagged versions are deleted before untagged ones"

# Pinned only by the lockfile given second: the index goes
prune -- --delete "$LOCK" ||
    fail "--delete with one lockfile failed: $(cat "$ERR")"
for name in kept-index kept-amd64 shared; do
    [[ " $(deleted) " == *" ${ID[$name]} "* ]] ||
        fail "$name was kept without a lockfile pinning its index"
done
[[ " $(deleted) " != *" ${ID[tag13]} "* ]] ||
    fail "the tag the remaining lockfile pins was deleted"
pass "every lockfile's pins are kept, and only theirs"

# ── a guest: artifacts attached to it ───────────

# Two old guests, one pinned, each with vm-info.json attached both ways
# oras can: by subject, and under a sha256-DIGEST tag naming an index of
# the artifacts
for guest in pinned unpinned; do
    g=$(manifest "guest-$guest")
    version guest-ionic "guest-$guest" "$g" 100 \
        "$(ctag $((300 + ${#guest})))"
    info=$(manifest "info-$guest" \
        "\"subject\":{\"digest\":\"$g\"},\"artifactType\":\"x\"")
    version guest-ionic "info-$guest" "$info" 100
    fallback=$(manifest "fallback-info-$guest")
    version guest-ionic "fallback-info-$guest" "$fallback" 100
    version guest-ionic "fallback-$guest" \
        "$(index "fallback-$guest" "$fallback")" 100 "sha256-${g#sha256:}"
done
# Some newer guests, so the two old ones aren't among the 10 newest
for i in $(seq 1 10); do
    version guest-ionic "guest$i" "$(manifest "guest$i")" $((40 + i)) \
        "$(ctag $((400 + i)))"
done
cat >>"$LOCK" <<EOF
GUEST_ARTIFACT_REPO=ghcr.io/rocm/rocm-ernic/guest-ionic
GUEST_ARTIFACT_TAG=$(ctag 306)
EOF
sed -i '/=docker.io/d; /=elsewhere/d' "$LOCK"

prune -- --delete "$LOCK" || fail "--delete failed: $(cat "$ERR")"
for name in guest-unpinned info-unpinned fallback-unpinned \
    fallback-info-unpinned; do
    [[ " $(deleted) " == *" ${ID[$name]} "* ]] ||
        fail "$name wasn't deleted with the guest: $(cat "$OUT")"
done
for name in guest-pinned info-pinned fallback-pinned \
    fallback-info-pinned; do
    [[ " $(deleted) " != *" ${ID[$name]} "* ]] ||
        fail "$name was deleted, though its guest is pinned"
done
pass "artifacts attached to a guest are kept with it, and deleted with it"

# ── what deletes nothing ────────────────────────

prune FAKE_CURL_FAIL=1 -- --delete "$LOCK" &&
    fail "it succeeded without reading the manifests"
[[ ! -s $LOG ]] || fail "it deleted without reading the manifests"
grep -q "couldn't read ghcr.io/rocm/rocm-ernic/" "$ERR" ||
    fail "it didn't say it couldn't read a manifest: $(cat "$ERR")"
prune FAKE_CURL_WRONG=1 -- --delete "$LOCK" &&
    fail "it succeeded with manifests that aren't what was asked for"
[[ ! -s $LOG ]] || fail "it deleted on the strength of the wrong manifests"
grep -q "the registry sent another manifest" "$ERR" ||
    fail "it didn't say the manifest was wrong: $(cat "$ERR")"
pass "a manifest it can't read, or the wrong one, deletes nothing"

prune FAKE_GH_FAIL=1 -- --delete "$LOCK" &&
    fail "it succeeded without listing the versions"
[[ ! -s $LOG ]] || fail "it deleted without listing the versions"
pass "versions it can't list delete nothing"

prune FAKE_DELETE_FAIL=1 -- --delete "$LOCK" &&
    fail "it succeeded though GitHub refused a deletion"
grep -q "needs the Admin role" "$ERR" ||
    fail "it didn't say why a deletion might be refused: $(cat "$ERR")"
pass "a refused deletion fails, and says why"

# Bad data from the API is refused rather than read wrongly. It's in the
# guest's package, read after the builder's, which has versions to
# delete: those aren't deleted either.
cp "$GH/guest-ionic.json" "$WORK/saved.json"
echo '[{"id":1,"name":"sha256:short","created_at":"2026-01-01T00:00:00Z"}]' \
    >>"$GH/guest-ionic.json"
prune -- --delete "$LOCK" && fail "a malformed digest was accepted"
grep -q "bad digest for version 1" "$ERR" || fail "it said: $(cat "$ERR")"
[[ ! -s $LOG ]] || fail "it deleted with a malformed digest listed"
cp "$WORK/saved.json" "$GH/guest-ionic.json"
pass "a malformed version deletes nothing, from any image"

# ── the lockfiles ───────────────────────────────

mv "$GH/guest-ionic.json" "$WORK/saved.json"
prune -- --delete "$LOCK" || fail "a missing package failed: $(cat "$ERR")"
grep -q "guest-ionic: not published; nothing to prune" "$OUT" ||
    fail "it didn't say the package isn't published: $(cat "$OUT")"
mv "$WORK/saved.json" "$GH/guest-ionic.json"
pass "a package not yet published is skipped"

sed -i "s/^GUEST_ARTIFACT_TAG=.*/GUEST_ARTIFACT_TAG=$(ctag 999)/" "$LOCK"
prune -- "$LOCK" || fail "a pin not published failed: $(cat "$ERR")"
grep -q "guest-ionic:$(ctag 999) is pinned but not published" "$ERR" ||
    fail "it didn't warn of a pin not published: $(cat "$ERR")"
pass "a pin not published is warned of"

# A package whose pin isn't published and whose every version is old and
# untagged: that's a mistake somewhere, not something to empty
version lonely lonely1 "$(manifest lonely1)" 100
version lonely lonely2 "$(manifest lonely2)" 100
printf 'X_IMAGE=ghcr.io/rocm/rocm-ernic/lonely\nX_TAG=%s\n' "$(ctag 5)" \
    >"$WORK/lonely.env"
prune -- --delete "$WORK/lonely.env" &&
    fail "it emptied a package"
grep -q "lonely: refusing to delete every version" "$ERR" ||
    fail "it didn't refuse to empty a package: $(cat "$ERR")"
[[ ! -s $LOG ]] || fail "it deleted from a package it wouldn't empty"
pass "a package isn't emptied"

printf 'GUEST_ARTIFACT_REPO=docker.io/x/y\nGUEST_ARTIFACT_TAG=z\n' \
    >"$WORK/other.env"
prune -- --delete "$WORK/other.env" ||
    fail "a lockfile naming no image here failed: $(cat "$ERR")"
[[ ! -s $LOG ]] || fail "it deleted with no image of ours pinned"
grep -q "nothing to prune" "$OUT" || fail "it didn't say so: $(cat "$OUT")"
pass "images in other registries aren't pruned"

printf 'BUILDER_IMAGE=ghcr.io/rocm/rocm-ernic/ci-builder\nBUILDER_TAG=a b\n' \
    >"$WORK/bad.env"
prune -- "$WORK/bad.env" && fail "a lockfile with a bad tag was accepted"
grep -q "BUILDER_TAG isn't a tag" "$ERR" || fail "it said: $(cat "$ERR")"
prune -- "$WORK/nosuch.env" && fail "a missing lockfile was accepted"
prune -- && fail "no lockfile was accepted"
prune -- --nosuch "$LOCK" && fail "an unknown option was accepted"
pass "bad lockfiles and arguments are refused"

echo "All prune-images tests passed."
