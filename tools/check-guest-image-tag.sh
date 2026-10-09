#!/usr/bin/env bash
#
# Fail if the guest image pin appears anywhere but ci/guest-image.env.
#
# The tag used to live in five files, each with a comment asking the next
# person to keep it equal to the others. Nothing checked, so the only way to
# find a missed copy was a lane quietly testing an older guest -- which
# presents as a flaky backend, not as a stale pin. This is the check that
# was missing.
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

if [ -z "${GUEST_ARTIFACT_TAG:-}" ]; then
    echo "error: ${PIN_FILE} does not define GUEST_ARTIFACT_TAG" >&2
    exit 2
fi

# A date-prefixed artifact tag: 20260916.g2cc8e79-vm.<release>-...-qcow2.
# Matching the shape rather than the current value means this keeps working
# across bumps, and catches a NEW stray copy rather than only the one that
# exists today.
PATTERN='[0-9]{8}\.g[0-9a-f]{7,}-vm\.[a-z0-9.-]+-qcow2'

stray=$(grep -rEn "${PATTERN}" . \
    --exclude-dir=.git \
    --exclude-dir=build \
    --exclude="$(basename "${PIN_FILE}")" \
    --exclude="$(basename "$0")" \
    2>/dev/null || true)

if [ -n "${stray}" ]; then
    echo "error: the guest image tag is pinned outside ${PIN_FILE}:" >&2
    while IFS= read -r line; do echo "  ${line}" >&2; done <<<"${stray}"
    cat >&2 <<EOF

${PIN_FILE} is the only place the pin may live. Consumers read it:

  shell            . ci/guest-image.env
  GitHub Actions   cat ci/guest-image.env >> "\$GITHUB_ENV"
  Ansible          lookup('ansible.builtin.ini', '<key> type=properties ...')

If you are adding a lane, use one of those rather than copying the tag.
EOF
    exit 1
fi

echo "✓ guest image tag is pinned only in ${PIN_FILE}"
echo "  ${GUEST_ARTIFACT_TAG}"
