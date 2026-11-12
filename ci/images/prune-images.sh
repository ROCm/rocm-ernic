#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# prune-images.sh
#
# Deletes the versions of the CI images in GHCR that nothing needs any
# more. Each image's package gains a version for every tag ensure-image.sh
# publishes, and another each time a build replaces the registry cache,
# IMAGE:buildcache, leaving the old one untagged; a guest image's are
# 1.6 GB each.
#
# The images are those the lockfiles name in ghcr.io/rocm/rocm-ernic, as
# PREFIX_IMAGE or PREFIX_REPO, with PREFIX_TAG. A version of one is kept
# if:
#   - a lockfile pins one of its tags;
#   - it's one of the 10 newest versions with a tag image-tag.sh made,
#     which ends in "-" and 12 hex digits;
#   - it's under 30 days old, which also covers a push in progress;
#   - it has a tag of any other kind, such as buildcache; or
#   - a version that's kept needs it: it's a manifest a kept index lists,
#     or an artifact attached to a kept version (oras attach), by the
#     artifact's subject or by a sha256-DIGEST tag. GHCR has no referrers
#     API, so oras falls back to that tag.
# Every other version is deleted: old tags, cache manifests buildcache
# no longer names, and whatever only those needed. A pruned tag that's
# wanted again, by an old branch or a long-lived pull request, is rebuilt
# by ensure-image.sh, which builds any tag the registry hasn't got.
#
# Nothing is deleted until every image's versions and manifests have been
# read, and any failure before then deletes nothing. Packages the
# lockfiles don't name aren't touched.
#
# Usage: prune-images.sh [--delete] LOCKFILE...
#   LOCKFILE  a copy of ci/guest-image.env whose pins to keep, such as
#             the default branch's
#   --delete  delete the versions; without it, only list them
#
# Needs gh, logged in with a token that can read the packages and, for
# --delete, has the Admin role on them, as the GITHUB_TOKEN of the
# repository that published them has. It also needs curl and jq. The
# packages are public, so manifests are read without credentials.

set -euo pipefail

PROG=$(basename -- "$0")

OWNER=rocm
NAMESPACE=rocm-ernic
REGISTRY=ghcr.io/$OWNER/$NAMESPACE
KEEP_NEWEST=10
MIN_AGE_DAYS=30

# Every manifest type a version can be, so the registry returns each as
# it was pushed, and its digest can be checked
ACCEPT=application/vnd.oci.image.index.v1+json
ACCEPT+=,application/vnd.oci.image.manifest.v1+json
ACCEPT+=,application/vnd.docker.distribution.manifest.list.v2+json
ACCEPT+=,application/vnd.docker.distribution.manifest.v2+json

die() {
    echo "$PROG: $*" >&2
    exit 1
}

usage() {
    echo "Usage: $PROG [--delete] LOCKFILE..." >&2
    exit 2
}

DELETE=false
LOCKFILES=()
while [[ $# -gt 0 ]]; do
    case $1 in
        --delete)
            DELETE=true
            shift
            ;;
        -h | --help) usage ;;
        -*) die "unknown option: $1" ;;
        *)
            LOCKFILES+=("$1")
            shift
            ;;
    esac
done
[[ ${#LOCKFILES[@]} -gt 0 ]] || usage

for tool in gh curl jq; do
    command -v "$tool" >/dev/null || die "$tool is needed and not installed"
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/prune-images-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# Each image in REGISTRY the lockfiles name, and the tags they pin, as
# " TAG TAG ..."
declare -A PINNED=()
for lock in "${LOCKFILES[@]}"; do
    [[ -f $lock && -r $lock ]] || die "can't read $lock"
    declare -A VAL=()
    while IFS= read -r line || [[ -n $line ]]; do
        if [[ $line =~ ^([A-Za-z_][A-Za-z0-9_]*)=(.*)$ ]]; then
            VAL[${BASH_REMATCH[1]}]=${BASH_REMATCH[2]}
        fi
    done <"$lock"
    for key in "${!VAL[@]}"; do
        [[ $key == *_TAG ]] || continue
        base=${key%_TAG}
        image=${VAL[${base}_IMAGE]:-${VAL[${base}_REPO]:-}}
        [[ $image == "$REGISTRY/"* ]] || continue
        [[ ${image#"$REGISTRY/"} =~ ^[a-z0-9][a-z0-9._-]*$ ]] ||
            die "$lock: not an image in $REGISTRY: '$image'"
        tag=${VAL[$key]}
        [[ $tag =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$ ]] ||
            die "$lock: $key isn't a tag: '$tag'"
        PINNED[$image]+=" $tag"
    done
    unset VAL
done

if [[ ${#PINNED[@]} -eq 0 ]]; then
    echo "No lockfile names an image in $REGISTRY; nothing to prune"
    exit 0
fi

NOW=$(date -u +%s)

# Decide which versions of an image to delete, report them, and add them
# to DOOMED.
#   $1  the image, REGISTRY/NAME
plan_image() {
    local image=$1
    local path=${image#ghcr.io/}
    local package=${image#"$REGISTRY/"}
    local api="/orgs/$OWNER/packages/container/$NAMESPACE%2F$package/versions"
    local pinned=${PINNED[$image]}

    # Its versions, from GitHub's API. A package that doesn't exist yet
    # has none to prune.
    if ! gh api --paginate "$api?per_page=100" >"$WORK/versions.json" \
        2>"$WORK/gh.err"; then
        if grep -q "HTTP 404" "$WORK/gh.err"; then
            echo "$image: not published; nothing to prune"
            return 0
        fi
        cat "$WORK/gh.err" >&2
        die "couldn't list the versions of $image"
    fi

    # One version to a line: its ID, digest, creation time in seconds
    # since the epoch and as a date, and tags. The tags come last because
    # tab is whitespace to read, which merges empty fields.
    jq -r '.[] | [(.id | tostring), .name,
        (.created_at | fromdateiso8601 | tostring), .created_at[0:10],
        ((.metadata.container.tags // []) | join(" "))] | @tsv' \
        "$WORK/versions.json" >"$WORK/versions.tsv" ||
        die "couldn't read the versions of $image"
    local -a ids=() digests=() created=() days=() tags=()
    local -A by_digest=() published=()
    local id digest when day tag_list t n=0
    while IFS=$'\t' read -r id digest when day tag_list; do
        [[ $id =~ ^[0-9]+$ ]] || die "$image: bad version ID: '$id'"
        [[ $digest =~ ^sha256:[0-9a-f]{64}$ ]] ||
            die "$image: bad digest for version $id: '$digest'"
        [[ $when =~ ^[0-9]+$ ]] ||
            die "$image: bad creation time for version $id: '$when'"
        ids[n]=$id
        digests[n]=$digest
        created[n]=$when
        days[n]=$day
        tags[n]=$tag_list
        by_digest[$digest]=$n
        for t in $tag_list; do
            published[$t]=1
        done
        n=$((n + 1))
    done <"$WORK/versions.tsv"
    if [[ $n -eq 0 ]]; then
        echo "$image: no versions; nothing to prune"
        return 0
    fi
    for t in $pinned; do
        if [[ -z ${published[$t]:-} ]]; then
            echo "$PROG: warning: $image:$t is pinned but not published" >&2
        fi
    done

    # Each version's manifest, for the manifests and artifacts it needs:
    # children[i] the versions an index lists, and referrers[DIGEST] the
    # versions attached to DIGEST. The pull token is anonymous: the
    # packages are public.
    local token
    token=$(curl -fsS --retry 3 \
        "https://ghcr.io/token?scope=repository:$path:pull" |
        jq -r '.token // empty') || token=""
    [[ -n $token ]] || die "couldn't get a token to read $image"
    local -a children=()
    local -A referrers=()
    local i sum links kind d
    for ((i = 0; i < n; i++)); do
        curl -fsS --retry 3 -H "Authorization: Bearer $token" \
            -H "Accept: $ACCEPT" -o "$WORK/manifest" \
            "https://ghcr.io/v2/$path/manifests/${digests[i]}" ||
            die "couldn't read $image@${digests[i]}"
        sum=$(sha256sum <"$WORK/manifest")
        [[ sha256:${sum%% *} == "${digests[i]}" ]] ||
            die "$image@${digests[i]}: the registry sent another manifest"
        # "subject DIGEST" and "child DIGEST" lines
        links=$(jq -r '(.subject.digest // empty | "subject " + .),
            ((.manifests // [])[] | "child " + .digest)' "$WORK/manifest") ||
            die "$image@${digests[i]}: not a manifest jq can read"
        children[i]=""
        while read -r kind d; do
            if [[ -z $kind ]]; then
                continue
            elif [[ $kind == subject ]]; then
                referrers[$d]+=" $i"
            elif [[ -n ${by_digest[$d]:-} ]]; then
                children[i]+=" ${by_digest[$d]}"
            fi
        done <<<"$links"
        for t in ${tags[i]}; do
            if [[ $t =~ ^sha256-([0-9a-f]{64})$ ]]; then
                referrers[sha256:${BASH_REMATCH[1]}]+=" $i"
            fi
        done
    done

    # The versions kept for themselves, and why
    local -a why=() newest=()
    local min_age=$((MIN_AGE_DAYS * 86400)) content line
    for ((i = 0; i < n; i++)); do
        why[i]=""
        content=false
        for t in ${tags[i]}; do
            if [[ " $pinned " == *" $t "* ]]; then
                why[i]=pinned
            elif [[ $t =~ ^sha256-[0-9a-f]{64}$ ]]; then
                : # an attached artifact's: kept with what it's attached to
            elif [[ $t =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]*-[0-9a-f]{12}$ ]]; then
                content=true
            elif [[ -z ${why[i]} ]]; then
                why[i]="tagged $t"
            fi
        done
        if [[ -z ${why[i]} && $((NOW - created[i])) -lt $min_age ]]; then
            why[i]="under $MIN_AGE_DAYS days old"
        fi
        if $content; then
            newest+=("${created[i]} ${ids[i]} $i")
        fi
    done
    if [[ ${#newest[@]} -gt 0 ]]; then
        while IFS= read -r line; do
            i=${line##* }
            if [[ -z ${why[i]} ]]; then
                why[i]="among the $KEEP_NEWEST newest"
            fi
        done < <(printf '%s\n' "${newest[@]}" | sort -k1,1nr -k2,2nr |
            head -n "$KEEP_NEWEST")
    fi

    # Then everything a kept version needs, and what that needs in turn
    local -a queue=()
    local j k
    for ((i = 0; i < n; i++)); do
        if [[ -n ${why[i]} ]]; then
            queue+=("$i")
        fi
    done
    for ((k = 0; k < ${#queue[@]}; k++)); do
        i=${queue[k]}
        for j in ${children[i]} ${referrers[${digests[i]}]:-}; do
            if [[ -z ${why[j]} ]]; then
                why[j]="needed by ${digests[i]:0:19}"
                queue+=("$j")
            fi
        done
    done

    # Report every version, newest first, and list those to delete:
    # tagged ones first, so a deletion that fails part way doesn't leave
    # a tag naming an index whose manifests are gone
    local -a doomed_tagged=() doomed_untagged=()
    local kept=0 action
    echo "$image:"
    while IFS= read -r line; do
        i=${line##* }
        if [[ -n ${why[i]} ]]; then
            action=keep
            kept=$((kept + 1))
        elif [[ -n ${tags[i]} ]]; then
            action=delete
            doomed_tagged+=("$i")
        else
            action=delete
            doomed_untagged+=("$i")
        fi
        printf '  %-6s %s  %s  %s%s\n' "$action" \
            "${days[i]}" "${digests[i]:0:19}" \
            "${tags[i]:-(untagged)}" "${why[i]:+  (${why[i]})}"
    done < <(for ((i = 0; i < n; i++)); do
        echo "${created[i]} ${ids[i]} $i"
    done | sort -k1,1nr -k2,2nr)

    [[ $kept -gt 0 ]] || die "$image: refusing to delete every version"
    echo "  $n versions: keeping $kept, deleting" \
        $((${#doomed_tagged[@]} + ${#doomed_untagged[@]}))
    for i in "${doomed_tagged[@]}" "${doomed_untagged[@]}"; do
        DOOMED+=("$api/${ids[i]} $image@${digests[i]}")
    done
}

# What to delete from every image, as "API-PATH IMAGE@DIGEST", all
# decided before any is deleted
DOOMED=()
mapfile -t IMAGES < <(printf '%s\n' "${!PINNED[@]}" | LC_ALL=C sort)
for image in "${IMAGES[@]}"; do
    plan_image "$image"
done

if [[ ${#DOOMED[@]} -eq 0 ]]; then
    echo "Nothing to delete"
    exit 0
fi
if ! $DELETE; then
    echo "Nothing deleted without --delete"
    exit 0
fi
for doomed in "${DOOMED[@]}"; do
    if ! gh api --method DELETE "${doomed%% *}" --silent 2>"$WORK/gh.err"; then
        cat "$WORK/gh.err" >&2
        die "GitHub refused to delete ${doomed#* } (${doomed%% *})." \
            "Deleting needs the Admin role on the package, which the" \
            "repository that published it has."
    fi
done
echo "Deleted ${#DOOMED[@]} versions"
