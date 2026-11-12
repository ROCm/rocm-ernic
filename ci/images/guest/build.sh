#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# build.sh
#
# Builds the CI guest image: Ubuntu's cloud image, on the pinned mainline
# kernel, with what the lanes need to build and test the ionic RDMA
# driver, and no way to log in until a boot's NoCloud seed authorizes a
# key (ci/boot-guest.sh makes one). It writes OUT/VM_NAME.qcow2 and
# OUT/vm-info.json, which describes it, and keeps the build's serial
# consoles and logs in OUT/logs.
#
# The pins come from pins.env through the environment, as
# ci/images/ensure-image.sh passes them; ensure-image.sh then runs
# check.sh on the result, and push.sh publishes it.
#
# It boots the guest once for each step, with KVM, so it runs in the CI
# builder image, as root or as a user who can open /dev/kvm:
#   stage      the cloud image boots once, with a NoCloud seed, while
#              cloud-init authorizes the build's key and installs
#              packages/ionic.txt; then a boot checks that went well and
#              takes unattended-upgrades out, and one installs the
#              mainline kernel and makes grub boot it
#   provision  provision/ionic.sh
#   trim       trim/kernel-only.sh, then the disk is compacted
#   verify     a boot of a throwaway overlay reads what vm-info.json
#              records, checks what every boot relies on, and runs
#              checks/ionic.sh
#   seal       removes every way in: the build's key, the password and
#              the host keys, and resets cloud-init, so the next boot is a
#              new instance
# Every step but the verify boot changes the disk.
#
# Usage: build.sh OUT
#   OUT  an empty or missing directory for the image
#
# Environment, beyond the pins:
#   GUEST_BUILD_VCPUS     the build's boots' vCPUs (default: 4)
#   GUEST_BUILD_MEM       and memory, in MiB (default: 4096)
#   GUEST_BUILD_SSH_PORT  port on 127.0.0.1 for their SSH (default: 2222)
#   CI_IMAGES_ROOT_CA     a root certificate for the build and the guest to
#                         trust, for a local build behind a proxy that
#                         re-signs TLS; vm-info.json records it, and
#                         ensure-image.sh won't publish such an image
#
# Needs qemu-system-x86_64 and qemu-img (the builder's are on PATH),
# cloud-localds, ssh, scp, ssh-keygen, curl, jq and timeout, and network
# access to cloud-images.ubuntu.com and kernel.ubuntu.com, and for the
# guest, to the Ubuntu archive.

set -euo pipefail

PROG=$(basename -- "$0")
GUEST_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ARCH=amd64

die() {
    echo "$PROG: $*" >&2
    exit 1
}

[[ $# -eq 1 ]] || {
    echo "Usage: $PROG OUT" >&2
    exit 2
}

# ── The pins and the environment ─────────────────────────────────────

for pin in RELEASE KERNEL_REF RDMA_CORE_VERSION VM_NAME VM_SIZE USERNAME \
    REBUILD; do
    [[ -n ${!pin:-} ]] ||
        die "$pin isn't set: pass the pins in pins.env, as ensure-image.sh does"
done
[[ $RELEASE =~ ^[a-z]+$ ]] || die "RELEASE isn't a codename: '$RELEASE'"
[[ $KERNEL_REF =~ ^v[0-9]+(\.[0-9]+)+$ ]] ||
    die "KERNEL_REF isn't a mainline version tag: '$KERNEL_REF'"
[[ $RDMA_CORE_VERSION =~ ^[0-9]+(\.[0-9]+)*$ ]] ||
    die "RDMA_CORE_VERSION isn't a version: '$RDMA_CORE_VERSION'"
[[ $VM_NAME =~ ^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$ ]] ||
    die "VM_NAME isn't a hostname: '$VM_NAME'"
[[ $VM_SIZE =~ ^[1-9][0-9]{0,4}$ ]] || die "VM_SIZE isn't a size: '$VM_SIZE'"
[[ $USERNAME =~ ^[a-z_][a-z0-9_-]{0,31}$ ]] ||
    die "USERNAME isn't a user name: '$USERNAME'"

export GUEST_BUILD_VCPUS=${GUEST_BUILD_VCPUS:-4}
export GUEST_BUILD_MEM=${GUEST_BUILD_MEM:-4096}
export GUEST_BUILD_SSH_PORT=${GUEST_BUILD_SSH_PORT:-2222}
for var in GUEST_BUILD_VCPUS GUEST_BUILD_MEM GUEST_BUILD_SSH_PORT; do
    [[ ${!var} =~ ^[1-9][0-9]{0,5}$ ]] || die "$var isn't a number"
done

for tool in cloud-localds curl jq qemu-img qemu-system-x86_64 scp sha256sum \
    ssh ssh-keygen timeout; do
    command -v "$tool" >/dev/null || die "$tool isn't on PATH"
done
# No falling back to TCG, which is about ten times slower
if ! { [[ -c /dev/kvm ]] && : 3<>/dev/kvm; } 2>/dev/null; then
    die "can't open /dev/kvm. Build in the builder image with" \
        "--device /dev/kvm, as root or as a member of /dev/kvm's group."
fi

mkdir -p -- "$1"
OUT=$(cd -- "$1" && pwd)
[[ -z $(ls -A -- "$OUT") ]] || die "$OUT isn't empty"
WORK=$OUT/work
LOGS=$OUT/logs
mkdir -p "$WORK" "$LOGS"
DISK=$WORK/$VM_NAME.qcow2
KEY=$WORK/id_ed25519

# Retries span the bursts of 503s kernel.ubuntu.com has, which have lasted
# 40 s; HTTP/1.1 because transfers from it have died with HTTP/2 stream
# resets
CURL=(curl -fsSL --http1.1 --retry 8 --retry-delay 10 --retry-all-errors)
ROOT_CA=false
if [[ -n ${CI_IMAGES_ROOT_CA:-} ]]; then
    [[ -f $CI_IMAGES_ROOT_CA && -r $CI_IMAGES_ROOT_CA ]] ||
        die "CI_IMAGES_ROOT_CA isn't a readable file: $CI_IMAGES_ROOT_CA"
    # Ubuntu's bundle plus the certificate, for every download here
    cat /etc/ssl/certs/ca-certificates.crt "$CI_IMAGES_ROOT_CA" \
        >"$WORK/ca-bundle.crt"
    CURL+=(--cacert "$WORK/ca-bundle.crt")
    ROOT_CA=true
fi

STARTED=$SECONDS
step() {
    echo "=== $PROG: $* ($((SECONDS - STARTED))s) ===" >&2
}

# Boot the disk and run a script in it, with probe-guest.sh. The script
# is the preamble, then each file.
#   $1   the step's name, for its console log
#   $2   the preamble
#   $3-  probe-guest.sh's options, then --, then the files
probe() {
    local name=$1 preamble=$2 script
    shift 2
    local opts=()
    while [[ $1 != -- ]]; do
        opts+=("$1")
        shift
    done
    shift
    script=$WORK/$name.sh
    {
        printf '%s\n' "$preamble"
        cat -- "$@"
    } >"$script"
    "$GUEST_DIR/probe-guest.sh" "${opts[@]}" \
        --console "$LOGS/$name.console.log" \
        "$DISK" "$USERNAME" "$KEY" "$script"
}

# A preamble that sets the shell options and variables
#   $@  the variables, as NAME=VALUE
preamble() {
    local var
    echo 'set -euo pipefail'
    for var in "$@"; do
        printf '%s=%q\n' "${var%%=*}" "${var#*=}"
    done
}

# ── stage: the disk, from the cloud image ────────────────────────────

# The cloud image, from its dated directory, which current/ points to and
# which doesn't change, so the image and its checksum can't come from
# different builds. HTTPS, and the checksum from the same server: this
# guards the transfer, not the server.
step "fetching the $RELEASE cloud image"
CLOUD_BASE=https://cloud-images.ubuntu.com/$RELEASE
CLOUD_SERIAL=$("${CURL[@]}" "$CLOUD_BASE/current/unpacked/build-info.txt" |
    sed -n 's/^serial=//p')
[[ $CLOUD_SERIAL =~ ^[0-9]{8}(\.[0-9]+)?$ ]] ||
    die "no serial in $CLOUD_BASE/current/unpacked/build-info.txt"
CLOUD_FILE=$RELEASE-server-cloudimg-$ARCH.img
CLOUD_URL=$CLOUD_BASE/$CLOUD_SERIAL/$CLOUD_FILE
sums=$("${CURL[@]}" "$CLOUD_BASE/$CLOUD_SERIAL/SHA256SUMS")
CLOUD_SHA256=$(awk -v f="$CLOUD_FILE" '$2 == f || $2 == "*" f { print $1 }' \
    <<<"$sums")
[[ $CLOUD_SHA256 =~ ^[0-9a-f]{64}$ ]] ||
    die "$CLOUD_BASE/$CLOUD_SERIAL/SHA256SUMS has no $CLOUD_FILE"
"${CURL[@]}" -o "$DISK" "$CLOUD_URL"
sha256sum -c --quiet - <<<"$CLOUD_SHA256  $DISK" ||
    die "$CLOUD_URL doesn't match its SHA256SUMS"
echo "$PROG: $CLOUD_URL, sha256 $CLOUD_SHA256" >&2
# Sparse: the guest's root filesystem grows into it on the first boot
qemu-img resize -q "$DISK" "${VM_SIZE}G"

# The packages, as a YAML list
PACKAGES=$WORK/packages
grep -vE '^[[:space:]]*(#|$)' "$GUEST_DIR/packages/ionic.txt" |
    sed 's/^/  - /' >"$PACKAGES"
PACKAGES_DIGEST=$(sha256sum <"$PACKAGES")
PACKAGES_DIGEST=${PACKAGES_DIGEST%% *}

# The build's own key, which no one else ever has: the seal removes it
# from the guest, and it goes with WORK
ssh-keygen -q -t ed25519 -N '' -C "rocm-ernic guest build" -f "$KEY"

# The first boot's NoCloud seed. cloud-init gives the image's default
# user, which has no password and passwordless sudo, the build's key;
# grows the root filesystem to the disk; trusts CI_IMAGES_ROOT_CA before
# it fetches anything; installs packages/ionic.txt, without
# recommendations; and powers the guest off. As in the image this
# replaced, services the lanes don't use are masked, the user's services
# run without a login, dmesg is readable without root, and services get
# 15 s to stop.
{
    echo '#cloud-config'
    echo "hostname: $VM_NAME"
    echo 'ssh_pwauth: false'
    echo 'users: [default]'
    echo 'ssh_authorized_keys:'
    echo "  - '$(<"$KEY.pub")'"
    if $ROOT_CA; then
        echo 'ca_certs:'
        echo '  trusted:'
        echo '    - |'
        sed 's/^/      /' "$CI_IMAGES_ROOT_CA"
    fi
    cat <<EOF
apt:
  conf: |
    APT::Install-Recommends "false";
    APT::Install-Suggests "false";
package_update: true
packages:
$(cat "$PACKAGES")
runcmd:
  - [systemctl, mask, multipathd.service, multipathd.socket, iscsid.service,
     iscsid.socket, snapd.service, snapd.socket, snapd.seeded.service]
  - [loginctl, enable-linger, $USERNAME]
write_files:
  - path: /etc/sysctl.d/10-kernel-hardening.conf
    content: |
      kernel.dmesg_restrict = 0
  - path: /etc/systemd/logind.conf.d/99-kill-user-processes.conf
    content: |
      [Login]
      KillUserProcesses=yes
  - path: /etc/systemd/system.conf.d/99-timeout.conf
    content: |
      [Manager]
      DefaultTimeoutStopSec=15s
power_state:
  mode: poweroff
  condition: true
EOF
} >"$WORK/user-data"
printf 'instance-id: %s-build\nlocal-hostname: %s\n' "$VM_NAME" "$VM_NAME" \
    >"$WORK/meta-data"
cloud-localds "$WORK/seed.img" "$WORK/user-data" "$WORK/meta-data" >&2

# QEMU exits when cloud-init powers the guest off. Not forever, for a
# guest that hangs, but an hour, for a slow mirror.
step "booting the cloud image while cloud-init installs packages"
status=0
timeout 3600 qemu-system-x86_64 \
    -machine q35,accel=kvm \
    -cpu host \
    -smp "$GUEST_BUILD_VCPUS" \
    -m "$GUEST_BUILD_MEM" \
    -drive "if=virtio,format=qcow2,file=$DISK,discard=unmap,detect-zeroes=unmap" \
    -drive "if=virtio,format=raw,file=$WORK/seed.img,readonly=on" \
    -netdev user,id=n0 \
    -device virtio-net-pci,netdev=n0 \
    -display none \
    -monitor none \
    -serial "file:$LOGS/first-boot.console.log" \
    </dev/null >"$LOGS/first-boot.qemu.log" 2>&1 || status=$?
if [[ $status -ne 0 ]]; then
    cat -- "$LOGS/first-boot.qemu.log" >&2
    tail -n 100 -- "$LOGS/first-boot.console.log" >&2 || true
    die "the first boot didn't power off by itself (status $status)"
fi

# cloud-init's verdict on the first boot. A package it couldn't install
# is only a warning; the verify step looks for each one.
#
# Ubuntu's cloud images run unattended-upgrades, whose timers take the
# dpkg lock while a later step is using apt, and which would replace the
# pinned kernel in a long-lived guest. Masked before it's purged: purging
# while apt-daily runs waits for the dpkg lock.
step "checking the first boot, and taking unattended-upgrades out"
cat >"$WORK/harden.body" <<'EOF'
sudo python3 - <<'PYTHON'
import json
import sys

with open("/var/lib/cloud/data/result.json", encoding="utf-8") as f:
    errors = json.load(f)["v1"]["errors"]
if errors:
    print("Error: cloud-init's first boot failed:", *errors, sep="\n  ",
          file=sys.stderr)
    sys.exit(1)
PYTHON
sudo systemctl mask --now apt-daily.timer apt-daily-upgrade.timer
sudo systemctl stop apt-daily.service apt-daily-upgrade.service || true
sudo systemctl mask unattended-upgrades.service || true
sudo DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=120 \
    purge -y unattended-upgrades
# For a later install that brings it back as a recommendation
sudo tee /etc/apt/apt.conf.d/99-no-unattended-upgrades >/dev/null <<'CONF'
APT::Periodic::Update-Package-Lists "0";
APT::Periodic::Unattended-Upgrade "0";
APT::Periodic::Download-Upgradeable-Packages "0";
APT::Periodic::AutocleanInterval "0";
CONF
EOF
probe harden "$(preamble)" --persist -- "$WORK/harden.body" >&2

# The mainline kernel: loose .debs in no repository, fetched here so the
# log names them, build stamp included, and checked against the
# directory's CHECKSUMS. Not the -64k variant, arm64's.
step "installing the $KERNEL_REF mainline kernel"
MAINLINE=https://kernel.ubuntu.com/mainline/$KERNEL_REF/$ARCH
DEBS=$WORK/debs
mkdir -p "$DEBS"
listing=$("${CURL[@]}" --max-time 120 "$MAINLINE/")
mapfile -t KERNEL_DEBS < <(grep -oE 'linux-[a-z-]+-[0-9][^"<>/]*\.deb' \
    <<<"$listing" | grep -v -- -64k | LC_ALL=C sort -u)
[[ ${#KERNEL_DEBS[@]} -gt 0 ]] || die "no kernel .debs at $MAINLINE/"
checksums=$("${CURL[@]}" --max-time 120 "$MAINLINE/CHECKSUMS")
# The SHA-256 section of CHECKSUMS
sha256s=$(sed -n '/^# Checksums-Sha256:/,/^#$/p' <<<"$checksums")
for deb in "${KERNEL_DEBS[@]}"; do
    sum=$(awk -v f="$deb" '$2 == f { print $1 }' <<<"$sha256s")
    [[ $sum =~ ^[0-9a-f]{64}$ ]] || die "$MAINLINE/CHECKSUMS has no $deb"
    echo "$PROG:   $deb" >&2
    "${CURL[@]}" -o "$DEBS/$deb" "$MAINLINE/$deb"
    sha256sum -c --quiet - <<<"$sum  $DEBS/$deb" ||
        die "$deb doesn't match $MAINLINE/CHECKSUMS"
done
# apt rather than dpkg -i, to resolve their dependencies. Then grub's
# default is pinned to the new kernel by its menu entry's ID, not left to
# whichever kernel sorts first: the release's own kernel is still
# installed until the trim, and booting it is the mistake this prevents.
# A grub.d file, because 50-cloudimg-settings.cfg overrides
# /etc/default/grub. grub.cfg is mode 0600, hence the sudo.
cat >"$WORK/kernel.body" <<'EOF'
sudo DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=120 \
    install -y /tmp/payload/*.deb
sudo update-grub
rm -rf /tmp/payload

KREL=""
for vmlinuz in /boot/vmlinuz-"${KPIN}"-*; do
    [[ -e ${vmlinuz} ]] && KREL=${vmlinuz#/boot/vmlinuz-}
done
if [[ -z ${KREL} ]]; then
    echo "Error: no /boot/vmlinuz-${KPIN}-* after installing the .debs" >&2
    exit 1
fi

cfg=$(sudo cat /boot/grub/grub.cfg)
# The menu entries' IDs, as $menuentry_id_option 'ID'
id_re="\\\$menuentry_id_option '\\([^']*\\)'"
SUBMENU_ID=$(sed -n "s/^submenu .*${id_re}.*/\\1/p" <<<"${cfg}")
SUBMENU_ID=${SUBMENU_ID%%$'\n'*}
ENTRY_ID=$(sed -n "s/^[[:space:]]*menuentry .*${id_re}.*/\\1/p" <<<"${cfg}" |
    grep -- "-${KREL}-advanced-") || ENTRY_ID=""
ENTRY_ID=${ENTRY_ID%%$'\n'*}
if [[ -z ${SUBMENU_ID} || -z ${ENTRY_ID} ]]; then
    echo "Error: no grub menu entry ID for ${KREL}" >&2
    grep -nE '^(submenu|[[:space:]]*menuentry)' <<<"${cfg}" >&2 || true
    exit 1
fi
sudo install -d -m 0755 /etc/default/grub.d
printf 'GRUB_DEFAULT="%s>%s"\n' "${SUBMENU_ID}" "${ENTRY_ID}" |
    sudo tee /etc/default/grub.d/99-pinned-kernel.cfg >/dev/null
sudo update-grub
echo "grub's default is ${KREL}"
EOF
probe kernel "$(preamble KPIN="${KERNEL_REF#v}")" --persist \
    --payload "$DEBS" -- \
    "$WORK/kernel.body" >&2
rm -rf -- "$DEBS"

# ── provision ────────────────────────────────────────────────────────

step "provisioning with provision/ionic.sh"
probe provision "$(preamble RELEASE="$RELEASE" \
    RDMA_CORE_VERSION="$RDMA_CORE_VERSION")" --persist -- \
    "$GUEST_DIR/provision/ionic.sh" >&2

# ── trim ─────────────────────────────────────────────────────────────

step "trimming with trim/kernel-only.sh"
probe trim "$(preamble)" --persist -- "$GUEST_DIR/trim/kernel-only.sh" >&2

# The trim freed blocks in the disk image, but it still has the slack of
# everything written and deleted since the cloud image; uncompressed, as
# guests write to it
# Allocated bytes, not the apparent size, which the trim's holes don't
# shrink
allocated() {
    local size
    size=$(du -B1 -- "$1")
    printf '%s\n' "${size%%[[:space:]]*}"
}
before=$(allocated "$DISK")
qemu-img convert -O qcow2 "$DISK" "$DISK.compact"
mv -- "$DISK.compact" "$DISK"
echo "$PROG: compacted the disk from $before to $(allocated "$DISK")" \
    "bytes allocated" >&2

# ── verify ───────────────────────────────────────────────────────────

# What vm-info.json records, read from the guest as PROBE_KEY=value lines,
# then what every boot relies on, then the flavour's checks. A boot of a
# throwaway overlay, so the disk is what the checks saw.
step "verifying"
cat >"$WORK/facts.body" <<'EOF'
# A command's output lines, sorted, on one line, separated by $1
list() {
    local sep=$1 items
    shift
    items=$("$@" | LC_ALL=C sort -V) || items=""
    printf '%s' "${items//$'\n'/${sep}}"
}

printf 'PROBE_KERNEL=%s\n' "$(uname -r)"
kernels=$(list , find /boot -maxdepth 1 -name 'vmlinuz-*' -printf '%f\n')
printf 'PROBE_KERNELS=%s\n' "${kernels//vmlinuz-/}"
initramfs=$(list , find /boot -maxdepth 1 -name 'initrd.img-*' -printf '%f\n')
printf 'PROBE_INITRAMFS=%s\n' "${initramfs//initrd.img-/}"

# The kernel grub boots by default, which isn't necessarily this one:
# this boot was chosen when the kernel was installed. The first `set
# default` isn't a ${...} reference; grub.cfg is mode 0600.
cfg=$(sudo cat /boot/grub/grub.cfg)
GRUB_DEFAULT_ID=$(sed -n 's/^[[:space:]]*set default="\(.*\)"$/\1/p' \
    <<<"${cfg}" | grep -v '\${') || GRUB_DEFAULT_ID=""
GRUB_DEFAULT_ID=${GRUB_DEFAULT_ID%%$'\n'*}
GRUB_DEFAULT_KERNEL=$(sed -n 's/^gnulinux-\(.*\)-advanced-.*/\1/p' \
    <<<"${GRUB_DEFAULT_ID##*>}")
printf 'PROBE_GRUB_DEFAULT_ID=%s\n' "${GRUB_DEFAULT_ID}"
printf 'PROBE_GRUB_DEFAULT_KERNEL=%s\n' "${GRUB_DEFAULT_KERNEL}"

. /etc/os-release
printf 'PROBE_OS_VERSION=%s\n' "${VERSION_ID:-}"
printf 'PROBE_OS_CODENAME=%s\n' "${VERSION_CODENAME:-}"
printf 'PROBE_DKMS=%s\n' "$(list ';' dkms status)"
printf 'PROBE_RDMA_CORE=%s\n' "$(dpkg-query -W -f='${Version}' rdma-core)"
printf 'PROBE_CLOUD_INIT=%s\n' \
    "$(dpkg-query -W -f='${Version}' cloud-init)"

# The account a boot's seed authorizes keys for must be cloud-init's
# default user, which the seed keeps (users: [default, ...]), or
# cloud-init makes a second account, without sudo
default_user=$(sudo python3 - <<'PY'
import glob
import yaml

name = ""
paths = sorted(glob.glob("/etc/cloud/cloud.cfg.d/*.cfg"))
for path in ["/etc/cloud/cloud.cfg"] + paths:
    with open(path, encoding="utf-8") as f:
        cfg = yaml.safe_load(f) or {}
    user = (cfg.get("system_info") or {}).get("default_user") or {}
    name = user.get("name", name)
print(name)
PY
)
if [[ ${default_user} != "${USERNAME}" ]]; then
    echo "Error: cloud-init's default user is '${default_user}'," \
        "not ${USERNAME}" >&2
    exit 1
fi

# Passwordless sudo, and no unattended-upgrades
sudo -n true
test ! -e /usr/bin/unattended-upgrade
test -f /etc/apt/apt.conf.d/99-no-unattended-upgrades
[[ $(systemctl is-enabled apt-daily.timer || true) == masked ]]
[[ $(systemctl is-enabled apt-daily-upgrade.timer || true) == masked ]]

# The pinned kernel, booted now and by default
if [[ $(uname -r) != "${KPIN}"-* ||
    ${GRUB_DEFAULT_KERNEL} != "${KPIN}"-* ]]; then
    echo "Error: booted $(uname -r), and grub's default is" \
        "'${GRUB_DEFAULT_KERNEL}', not the pinned ${KPIN}" >&2
    exit 1
fi

# Every package packages/ionic.txt asks for: cloud-init logs one it can't
# find and carries on
missing=""
for pkg in ${WANTED}; do
    status=$(dpkg-query -W -f='${Status}' "${pkg}" 2>/dev/null) || status=""
    if [[ ${status} != "install ok installed" ]]; then
        missing+=" ${pkg}"
    fi
done
if [[ -n ${missing} ]]; then
    echo "Error: packages missing from the guest:${missing}" >&2
    exit 1
fi

echo "Running checks/ionic.sh" >&2
set -x
EOF
wanted=$(grep -vE '^[[:space:]]*(#|$)' "$GUEST_DIR/packages/ionic.txt" |
    tr '\n' ' ')
printf '\nset +x\necho PROBE_OK\n' >"$WORK/facts.end"
status=0
probe_out=$(probe verify "$(preamble KPIN="${KERNEL_REF#v}" \
    KERNEL_REF="$KERNEL_REF" USERNAME="$USERNAME" WANTED="$wanted")" -- \
    "$WORK/facts.body" "$GUEST_DIR/checks/ionic.sh" "$WORK/facts.end") ||
    status=$?
printf '%s\n' "$probe_out" >&2
if [[ $status -ne 0 ]] || ! grep -qx PROBE_OK <<<"$probe_out"; then
    die "the guest failed its checks"
fi

# A fact the guest reported. They're data, so they reach vm-info.json only
# through jq --arg.
#   $1  its name, without PROBE_
fact() {
    local line value=""
    while IFS= read -r line; do
        if [[ $line == "PROBE_$1="* ]]; then
            value=${line#*=}
            break
        fi
    done <<<"$probe_out"
    printf '%s\n' "$value"
}

# ── seal ─────────────────────────────────────────────────────────────

# Nothing that lets anyone in may ship. The guest's next boot takes its
# keys from a seed, as a new instance: cloud-init authorizes them for the
# default user, which keeps its sudo, and makes new host keys.
#
# The build's key leaves authorized_keys while it's logged in with it, so
# the guest is powered off from here, a few seconds after this exits, not
# by a new login. sshd takes the first value it reads for a setting, so
# the 01- file outranks any other; 50-cloud-init.conf allowed passwords.
# cloud-init clean resets cloud-init to a fresh instance, and with
# --machine-id, empties /etc/machine-id for systemd to make a new one.
step "sealing"
cat >"$WORK/seal.body" <<'EOF'
sudo passwd -l "${USERNAME}"
sudo passwd -l root
home=$(getent passwd "${USERNAME}" | cut -d: -f6)
sudo truncate -s 0 "${home}/.ssh/authorized_keys"
sudo rm -f /root/.ssh/authorized_keys
sudo tee /etc/ssh/sshd_config.d/01-rocm-ernic.conf >/dev/null <<'CONF'
# The guest image ships no password and no key: log in with a key a
# boot's NoCloud seed authorizes
PasswordAuthentication no
KbdInteractiveAuthentication no
CONF
sudo rm -f /etc/ssh/sshd_config.d/50-cloud-init.conf
sudo sh -c 'rm -f /etc/ssh/ssh_host_*'
sudo rm -rf /tmp/payload
if sudo cloud-init clean --help | grep -q -- --machine-id; then
    sudo cloud-init clean --logs --seed --machine-id
else
    sudo cloud-init clean --logs --seed
    sudo truncate -s 0 /etc/machine-id
fi
sudo systemd-run --quiet --on-active=3 systemctl poweroff
EOF
probe seal "$(preamble USERNAME="$USERNAME")" --persist -- \
    "$WORK/seal.body" >&2

# ── vm-info.json ─────────────────────────────────────────────────────

step "writing vm-info.json"
mv -- "$DISK" "$OUT/$VM_NAME.qcow2"
DISK=$OUT/$VM_NAME.qcow2
disk_info=$(qemu-img info --output=json -- "$DISK")
qemu_version=$(qemu-system-x86_64 --version)
qemu_version=${qemu_version%%$'\n'*}
qemu_commit=unknown
QEMU_DIR=$(dirname -- "$(command -v qemu-system-x86_64)")
if [[ -r $QEMU_DIR/../share/qemu-commit.txt ]]; then
    qemu_commit=$(<"$QEMU_DIR/../share/qemu-commit.txt")
fi
revision=${GITHUB_SHA:-$(git -C "$GUEST_DIR" rev-parse HEAD 2>/dev/null ||
    echo unknown)}

# The list facts are comma-separated
jq -n \
    --arg vm_name "$VM_NAME" \
    --arg username "$USERNAME" \
    --arg image "$VM_NAME.qcow2" \
    --argjson disk "$disk_info" \
    --argjson vm_size "$VM_SIZE" \
    --arg release "$RELEASE" \
    --arg arch "$ARCH" \
    --arg kernel "$(fact KERNEL)" \
    --arg kernels "$(fact KERNELS)" \
    --arg initramfs "$(fact INITRAMFS)" \
    --arg grub_id "$(fact GRUB_DEFAULT_ID)" \
    --arg grub_kernel "$(fact GRUB_DEFAULT_KERNEL)" \
    --arg os_version "$(fact OS_VERSION)" \
    --arg os_codename "$(fact OS_CODENAME)" \
    --arg dkms "$(fact DKMS)" \
    --arg rdma_core "$(fact RDMA_CORE)" \
    --arg cloud_init "$(fact CLOUD_INIT)" \
    --arg cloud_url "$CLOUD_URL" \
    --arg cloud_serial "$CLOUD_SERIAL" \
    --arg cloud_sha256 "$CLOUD_SHA256" \
    --arg packages_digest "$PACKAGES_DIGEST" \
    --arg kernel_ref "$KERNEL_REF" \
    --arg kernel_debs "${KERNEL_DEBS[*]}" \
    --argjson root_ca "$ROOT_CA" \
    --arg qemu_version "$qemu_version" \
    --arg qemu_commit "$qemu_commit" \
    --arg revision "$revision" \
    --arg timestamp "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    --argjson vcpus "$GUEST_BUILD_VCPUS" \
    --argjson mem "$GUEST_BUILD_MEM" \
    '
    def list: if . == "" then [] else split(",") end;
    {
      schema_version: 10,
      vm_name: $vm_name,
      flavour: "ionic",
      username: $username,
      login: {user: $username, method: "nocloud-seed"},
      image_path: $image,
      image_format: $disk.format,
      image_size_bytes: $disk["actual-size"],
      image_virtual_size_bytes: $disk["virtual-size"],
      vm_size_gb: $vm_size,
      release: $release,
      architecture: $arch,
      kernel_release: $kernel,
      kernels: {
        booted: $kernel,
        default: $grub_kernel,
        default_grub_id: $grub_id,
        installed: ($kernels | list),
        initramfs: ($initramfs | list)
      },
      os: {distribution: "ubuntu", version: $os_version,
           codename: $os_codename},
      dkms: ($dkms | list),
      rdma_core: $rdma_core,
      cloud_init: $cloud_init,
      cloud_image: {url: $cloud_url, serial: $cloud_serial,
                    sha256: $cloud_sha256},
      provisioning: {
        vm_packages: "ionic.txt",
        packages_digest: $packages_digest,
        kernel_ref: $kernel_ref,
        kernel_debs: $kernel_debs,
        provision: "ionic.sh",
        trim: "kernel-only.sh",
        checks: "ionic.sh",
        unattended_upgrades: "purged",
        sealed: true,
        root_ca: $root_ca
      },
      build_info: {
        qemu_version: $qemu_version,
        qemu_commit: $qemu_commit,
        source_revision: $revision,
        build_timestamp: $timestamp,
        build_vcpus: $vcpus,
        build_vmem_mib: $mem
      }
    }' >"$OUT/vm-info.json"
cat -- "$OUT/vm-info.json" >&2

rm -rf -- "$WORK"
step "built $DISK"
