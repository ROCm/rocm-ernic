#!/usr/bin/env bash
#
# Fail if the guest image pin or the builder image pin appears anywhere but
# ci/guest-image.env.
#
# The guest tag used to live in five files, each with a comment asking the
# next person to keep it equal to the others. Nothing checked, so the only
# way to find a missed copy was a lane quietly testing an older guest --
# which presents as a flaky backend, not as a stale pin. This is the check
# that was missing. The builder's tag gets the same treatment, except in
# a workflow's container `image:` line naming exactly the pinned builder:
# the security scan wants those written out, and
# `ci/images/image-tag.sh --write builder` keeps them equal to the pin.
#
# Only files git tracks are searched, which is everything CI sees; build
# trees and notes in a working copy aren't pins.
#
# Run from the repository root:
#
#   tools/check-guest-image-tag.sh
set -euo pipefail

PIN_FILE="ci/guest-image.env"

if [ ! -f "${PIN_FILE}" ]; then
    echo "error: ${PIN_FILE} not found; run from the repository root" >&2
    exit 2
fi

# shellcheck source=/dev/null
. "${PIN_FILE}"

for key in GUEST_ARTIFACT_TAG BUILDER_IMAGE BUILDER_TAG; do
    if [ -z "${!key:-}" ]; then
        echo "error: ${PIN_FILE} does not define ${key}" >&2
        exit 2
    fi
done

# Each pin's shape rather than its current value, so this keeps working
# across bumps, and catches a NEW stray copy rather than only the one that
# exists today:
#   - a date-prefixed guest artifact tag,
#     20260916.g2cc8e79-vm.<release>-...-qcow2;
#   - a builder tag, qemu<version or .sha>-vfu.<sha>-<12 hex digits>, as
#     ci/images/image-tag.sh makes them.
PATTERN='[0-9]{8}\.g[0-9a-f]{7,}-vm\.[a-z0-9.-]+-qcow2'
PATTERN+='|qemu([0-9]+(\.[0-9]+)+|\.[0-9a-f]{7})-vfu\.[0-9a-f]{7}-[0-9a-f]{12}'

# Excluded by path, not by name, so a copy at some-lane/guest-image.env --
# the stray most likely to be created, since a new lane copies the file it
# already knows the name of -- is still caught. Same for this script: only
# this path is the checker, anything else matching the pattern is a stray.
SELF="${BASH_SOURCE[0]#./}"
# git grep exits 1 for no match, and above that for an error
status=0
stray=$(git grep -nE "${PATTERN}" -- . \
    ":(exclude)${PIN_FILE}" ":(exclude)${SELF}") || status=$?
if [ "${status}" -gt 1 ]; then
    echo "error: git grep failed; run from a git checkout" >&2
    exit 2
fi

# Drop the workflows' image: lines naming the pinned builder, and only
# those: one naming another tag is still a stray. A line is read as
# ci/images/image-tag.sh reads it, to be one it rewrites: nothing but
# `image:`, whitespace and the reference.
pin_re='^[[:space:]]*image:[[:space:]]+([^[:space:]]+)$'
kept=""
while IFS= read -r line; do
    [ -n "${line}" ] || continue
    file=${line%%:*}
    text=${line#*:*:}
    if [[ ${file} == .github/workflows/* && ${text} =~ ${pin_re} &&
        ${BASH_REMATCH[1]} == "${BUILDER_IMAGE}:${BUILDER_TAG}" ]]; then
        continue
    fi
    kept+="${line}"$'\n'
done <<<"${stray}"
stray=${kept%$'\n'}

if [ -n "${stray}" ]; then
    echo "error: an image tag is pinned outside ${PIN_FILE}:" >&2
    while IFS= read -r line; do echo "  ${line}" >&2; done <<<"${stray}"
    cat >&2 <<EOF

${PIN_FILE} is the only place the pins may live. Consumers read it:

  shell            . ci/guest-image.env
  GitHub Actions   grep -E '^[A-Za-z_][A-Za-z0-9_]*=' ci/guest-image.env
                     >> "\$GITHUB_ENV"
  Ansible          lookup('ansible.builtin.ini', '<key> type=properties ...')

If you are adding a lane, use one of those rather than copying the tag. A
lane's container line names the builder in full, as the others do, and
ci/images/image-tag.sh --write builder updates it with them.
EOF
    exit 1
fi

echo "✓ the image tags are pinned only in ${PIN_FILE}"
echo "  guest:   ${GUEST_ARTIFACT_TAG}"
echo "  builder: ${BUILDER_IMAGE}:${BUILDER_TAG}"
