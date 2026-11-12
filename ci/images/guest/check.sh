#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# check.sh
#
# Checks a freshly built guest image before ci/images/ensure-image.sh
# publishes it, by booting it as the lanes and `ernicctl vm-launch` do:
# with ci/boot-guest.sh, whose NoCloud seed authorizes a key made for the
# boot. The boot is a snapshot, so the image is left as it was. It fails
# unless:
#   - the boot's key logs in, and is the only key the account trusts;
#   - sudo needs no password;
#   - the account's password is locked, and sshd refuses passwords;
#   - the guest made host keys of its own, the image having none;
#   - cloud-init finished without an error or a warning;
#   - the guest runs the kernel vm-info.json names.
# So an image whose seal doesn't hold, or that the helper can't boot,
# fails here rather than in a lane.
#
# Usage: check.sh OUT
#   OUT  the directory build.sh wrote the image to
#
# The pins come from pins.env through the environment, as ensure-image.sh
# passes them; GUEST_BUILD_VCPUS, GUEST_BUILD_MEM and GUEST_BUILD_SSH_PORT
# size the boot and choose its port, as for build.sh.

set -euo pipefail

PROG=$(basename -- "$0")
GUEST_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
BOOT_GUEST=$(cd -- "$GUEST_DIR/../.." && pwd)/boot-guest.sh

die() {
    echo "$PROG: $*" >&2
    exit 1
}

[[ $# -eq 1 ]] || {
    echo "Usage: $PROG OUT" >&2
    exit 2
}
OUT=$(cd -- "$1" && pwd)

for pin in VM_NAME USERNAME; do
    [[ -n ${!pin:-} ]] ||
        die "$pin isn't set: pass the pins in pins.env, as ensure-image.sh does"
done
DISK=$OUT/$VM_NAME.qcow2
INFO=$OUT/vm-info.json
[[ -f $DISK ]] || die "no image at $DISK"
[[ -f $INFO ]] || die "no vm-info.json in $OUT"
[[ -x $BOOT_GUEST ]] || die "$BOOT_GUEST is missing"
KERNEL=$(jq -r '.kernel_release // ""' "$INFO")
[[ -n $KERNEL ]] || die "$INFO names no kernel_release"

mkdir -p "$OUT/logs"
BOOT_DIR=$(mktemp -d "$OUT/check-XXXXXX")
QEMU_PID=""
cleanup() {
    if [[ -n $QEMU_PID ]]; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        for _ in {1..20}; do
            kill -0 "$QEMU_PID" 2>/dev/null || break
            sleep 0.5
        done
        kill -KILL "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf -- "$BOOT_DIR"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo "$PROG: booting $DISK with $BOOT_GUEST" >&2
status=0
boot=$("$BOOT_GUEST" --disk "$DISK" --name "$VM_NAME-check" \
    --ssh-port "${GUEST_BUILD_SSH_PORT:-2222}" --out "$BOOT_DIR" \
    --user "$USERNAME" --cpus "${GUEST_BUILD_VCPUS:-4}" \
    --mem "${GUEST_BUILD_MEM:-4096}" --snapshot --timeout 600 \
    --console "$OUT/logs/check.console.log") || status=$?
# boot-guest.sh's results, k=v
declare -A RESULT=()
while IFS='=' read -r key value; do
    [[ -n $key ]] && RESULT[$key]=$value
done <<<"$boot"
QEMU_PID=${RESULT[pid]:-}
[[ $status -eq 0 ]] || die "the guest didn't boot and accept the boot's key"
KEY=${RESULT[key]:-}
PORT=${RESULT[port]:-}
[[ -n $KEY && -n $PORT && ${RESULT[user]:-} == "$USERNAME" ]] ||
    die "boot-guest.sh's results are incomplete: $boot"

SSH=(
    ssh -F /dev/null -i "$KEY" -o IdentitiesOnly=yes -o BatchMode=yes
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
    -o LogLevel=ERROR -o ConnectTimeout=10 -p "$PORT"
    "$USERNAME@127.0.0.1"
)

# What the guest says about itself, as CHECK_KEY=value lines. Each command
# that could fail is allowed to, so every answer is collected. Copied in
# and run from there, so no command in it can read the rest of it.
"${SSH[@]}" 'cat >/tmp/check-guest.sh' <<'EOF' ||
cloud_init=0
timeout 300 cloud-init status --wait >/dev/null 2>&1 || cloud_init=$?
echo "CHECK_CLOUD_INIT=$cloud_init"
keys=$(grep -vE '^[[:space:]]*(#|$)' ~/.ssh/authorized_keys |
    awk '{ print $1, $2 }') || keys=""
echo "CHECK_KEYS=${keys//$'\n'/;}"
sudo=no
sudo -n true 2>/dev/null && sudo=yes
echo "CHECK_SUDO=$sudo"
echo "CHECK_PASSWD=$(sudo -n passwd -S "$USER" 2>/dev/null |
    awk '{ print $2 }')"
echo "CHECK_SSHD_PASSWORDS=$(sudo -n sshd -T 2>/dev/null |
    awk '$1 == "passwordauthentication" { print $2 }')"
host_keys=0
for f in /etc/ssh/ssh_host_*_key.pub; do
    [[ -e $f ]] && host_keys=$((host_keys + 1))
done
echo "CHECK_HOST_KEYS=$host_keys"
echo "CHECK_KERNEL=$(uname -r)"
EOF
    die "couldn't copy the checks to the guest"
facts=$("${SSH[@]}" bash /tmp/check-guest.sh </dev/null) ||
    die "couldn't run the checks in the guest"
printf '%s\n' "$facts" >&2

declare -A FACT=()
while IFS='=' read -r key value; do
    [[ $key == CHECK_* ]] && FACT[${key#CHECK_}]=$value
done <<<"$facts"

read -r want_type want_key _ <"$KEY.pub"
problems=()
[[ ${FACT[CLOUD_INIT]:-} == 0 ]] ||
    problems+=("cloud-init reported an error or a warning \
(status ${FACT[CLOUD_INIT]:-none})")
[[ ${FACT[KEYS]:-} == "$want_type $want_key" ]] ||
    problems+=("the account trusts keys other than the boot's: \
${FACT[KEYS]:-none}")
[[ ${FACT[SUDO]:-} == yes ]] || problems+=("sudo asks for a password")
[[ ${FACT[PASSWD]:-} == L ]] ||
    problems+=("the account's password isn't locked: ${FACT[PASSWD]:-unknown}")
[[ ${FACT[SSHD_PASSWORDS]:-} == no ]] ||
    problems+=("sshd's PasswordAuthentication is \
'${FACT[SSHD_PASSWORDS]:-unknown}'")
[[ ${FACT[HOST_KEYS]:-} =~ ^[1-9][0-9]*$ ]] ||
    problems+=("the guest has no host keys")
[[ ${FACT[KERNEL]:-} == "$KERNEL" ]] ||
    problems+=("the guest runs ${FACT[KERNEL]:-nothing}, not $KERNEL")

if [[ ${#problems[@]} -gt 0 ]]; then
    "${SSH[@]}" cloud-init status --long </dev/null >&2 || true
    for problem in "${problems[@]}"; do
        echo "$PROG: $problem" >&2
    done
    die "$DISK failed its checks"
fi
echo "$PROG: $DISK boots with a key from its seed and no other way in" >&2
