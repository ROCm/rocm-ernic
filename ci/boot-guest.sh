#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# boot-guest.sh
#
# Boots a guest image under QEMU with an SSH key made for this boot, and
# returns once the guest accepts that key, leaving QEMU running.
#
# The key reaches the guest through a NoCloud seed: a small disk labelled
# cidata holding user-data and meta-data, which cloud-init reads at boot.
# Each boot has a new instance-id, so cloud-init treats it as a new
# instance: it authorizes the keys for --user and makes new host keys. The
# seed removes no key, so an image that already trusts one goes on
# trusting it. An image without cloud-init ignores the seed, so it never
# accepts the boot's key, and the wait for SSH times out.
#
# The guest's SSH port is forwarded from 127.0.0.1 only.
#
# On success, k=v lines go to stdout:
#   pid=   QEMU's process ID
#   key=   the private key made for this boot
#   user=  the account to log in as
#   port=  the forwarded SSH port, on 127.0.0.1
# Everything else goes to stderr. On failure, QEMU is stopped.
#
# ernicctl vm-launch runs the installed copy,
# libexec/rocm-ernic/rocm-ernic-boot-guest.
#
# Needs QEMU (with vfio-user support for --vfio-user), cloud-localds
# (cloud-image-utils), ssh, ssh-keygen and setsid.

set -euo pipefail

PROG=$(basename -- "$0")

usage() {
    cat <<EOF
Usage: $PROG --disk FILE --name NAME --ssh-port PORT --out DIR [options]
       [-- QEMU ARGS...]

  --disk FILE          qcow2 boot disk
  --name NAME          guest hostname, and the stem of --nvme image names
  --ssh-port PORT      port on 127.0.0.1 forwarded to the guest's port 22
  --out DIR            where the key, seed and logs go (made mode 0700)

Guest:
  --user USER          account the keys are authorized for, and to log
                       in as (default: ubuntu); one that isn't the image's
                       default user is made, without sudo, if missing,
                       unless a group already has its name
  --authorize FILE     also authorize the public keys in FILE (repeatable)
  --accel kvm|tcg      accelerator (default: kvm)
  --cpu MODEL          CPU model (default: host with kvm, qemu64 with tcg)
  --cpus N             vCPUs (default: 2)
  --mem MB             memory in MiB (default: 4096)
  --snapshot           discard writes to the disk when QEMU exits
  --shared-backing     don't lock the disk's backing file, which other
                       guests' overlays share
  --timeout SECS       how long to wait for SSH (default: 600)

QEMU:
  --qemu BINARY        qemu-system-x86_64 to run (default: from PATH)
  --qemu-img BINARY    qemu-img for --nvme (default: beside --qemu, else
                       from PATH)
  --console FILE       serial console log (default: DIR/console.log)
  --qmp SOCKET         QMP socket to create
  --qga SOCKET         socket to create for the guest agent's channel

Devices:
  --vfio-user SOCKET   vfio-user device, on root port pcie-vfu.1
  --vfio-user-id ID    its QEMU device ID (default: vfio-user0)
  --mcast ADDR:PORT    second NIC on a UDP multicast segment
  --hostfwd RULE       another user-network forward, as QEMU's hostfwd=
                       takes it (repeatable), e.g. tcp:127.0.0.1:9150-:9100
  --pci-hostdev BDF    pass a host PCI function bound to vfio-pci through,
                       on its own root port (repeatable)
  --nvme N             N emulated NVMe SSDs (default: 0), backed by sparse
                       1 TiB images NAME-nvmeI.qcow2, made if missing
  --nvme-dir DIR       where those images are (default: the disk's directory)
  --nvme-aio MODE      their AIO: io_uring, native or threads (default: the
                       first of these QEMU can open them with)
  --pci-mmio-bridge    add the pci-mmio-bridge device (from a QEMU fork)

Arguments after -- go to QEMU as they are.
EOF
}

die() {
    echo "$PROG: $*" >&2
    exit 1
}

# Require a value after an option.
#   $1  the option
#   $2  how many arguments are left, the option's included
need_value() {
    if [[ $2 -lt 2 ]]; then
        die "$1 needs a value"
    fi
}

# Check a whole number, written without a sign or leading zeros.
#   $1  the option, for the message
#   $2  the value
#   $3  the smallest value allowed
#   $4  the largest
check_uint() {
    if [[ ! $2 =~ ^(0|[1-9][0-9]{0,8})$ ]]; then
        die "$1 isn't a number: '$2'"
    fi
    if ((10#$2 < $3 || 10#$2 > $4)); then
        die "$1 must be from $3 to $4: $2"
    fi
}

# Print a path, made absolute, that QEMU can take in an option value.
# QEMU reads a comma as the end of the value, and a name with a colon
# before any slash as a protocol, which an absolute path can't have.
#   $1  the option, for the message
#   $2  the path
qemu_path() {
    local path=$2

    if [[ -z $path ]]; then
        die "$1 is empty"
    fi
    if [[ $path != /* ]]; then
        path="$PWD/$path"
    fi
    if [[ $path == *,* ]]; then
        die "$1 can't contain a comma: $path"
    fi
    if [[ $path == *[[:cntrl:]]* ]]; then
        die "$1 can't contain a control character"
    fi
    printf '%s\n' "$path"
}

# Remove a socket left by an earlier QEMU, which would stop this one from
# creating it. Anything else at the path is refused.
#   $1  the option, for the message
#   $2  the path
clear_socket() {
    if [[ -S $2 ]]; then
        rm -f -- "$2"
    elif [[ -e $2 || -L $2 ]]; then
        die "$1 exists and isn't a socket: $2"
    fi
}

DISK=""
NAME=""
SSH_PORT=""
OUT=""
GUEST_USER=ubuntu
AUTHORIZE=()
ACCEL=kvm
CPU=""
CPUS=2
MEM=4096
SNAPSHOT=false
SHARED_BACKING=false
TIMEOUT=600
QEMU=qemu-system-x86_64
QEMU_IMG=""
CONSOLE=""
QMP=""
QGA=""
VFIO_USER=""
VFIO_USER_ID=vfio-user0
MCAST=""
HOSTFWDS=()
HOSTDEVS=()
NVME=0
NVME_DIR=""
NVME_AIO=""
MMIO_BRIDGE=false
EXTRA=()

while [[ $# -gt 0 ]]; do
    case $1 in
        --disk)
            need_value "$1" $#
            DISK=$2
            shift 2
            ;;
        --name)
            need_value "$1" $#
            NAME=$2
            shift 2
            ;;
        --ssh-port)
            need_value "$1" $#
            SSH_PORT=$2
            shift 2
            ;;
        --out)
            need_value "$1" $#
            OUT=$2
            shift 2
            ;;
        --user)
            need_value "$1" $#
            GUEST_USER=$2
            shift 2
            ;;
        --authorize)
            need_value "$1" $#
            AUTHORIZE+=("$2")
            shift 2
            ;;
        --accel)
            need_value "$1" $#
            ACCEL=$2
            shift 2
            ;;
        --cpu)
            need_value "$1" $#
            CPU=$2
            shift 2
            ;;
        --cpus)
            need_value "$1" $#
            CPUS=$2
            shift 2
            ;;
        --mem)
            need_value "$1" $#
            MEM=$2
            shift 2
            ;;
        --snapshot)
            SNAPSHOT=true
            shift
            ;;
        --shared-backing)
            SHARED_BACKING=true
            shift
            ;;
        --timeout)
            need_value "$1" $#
            TIMEOUT=$2
            shift 2
            ;;
        --qemu)
            need_value "$1" $#
            QEMU=$2
            shift 2
            ;;
        --qemu-img)
            need_value "$1" $#
            QEMU_IMG=$2
            shift 2
            ;;
        --console)
            need_value "$1" $#
            CONSOLE=$2
            shift 2
            ;;
        --qmp)
            need_value "$1" $#
            QMP=$2
            shift 2
            ;;
        --qga)
            need_value "$1" $#
            QGA=$2
            shift 2
            ;;
        --vfio-user)
            need_value "$1" $#
            VFIO_USER=$2
            shift 2
            ;;
        --vfio-user-id)
            need_value "$1" $#
            VFIO_USER_ID=$2
            shift 2
            ;;
        --mcast)
            need_value "$1" $#
            MCAST=$2
            shift 2
            ;;
        --hostfwd)
            need_value "$1" $#
            HOSTFWDS+=("$2")
            shift 2
            ;;
        --pci-hostdev)
            need_value "$1" $#
            HOSTDEVS+=("$2")
            shift 2
            ;;
        --nvme)
            need_value "$1" $#
            NVME=$2
            shift 2
            ;;
        --nvme-dir)
            need_value "$1" $#
            NVME_DIR=$2
            shift 2
            ;;
        --nvme-aio)
            need_value "$1" $#
            NVME_AIO=$2
            shift 2
            ;;
        --pci-mmio-bridge)
            MMIO_BRIDGE=true
            shift
            ;;
        --)
            shift
            EXTRA=("$@")
            break
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            die "unknown option: $1"
            ;;
    esac
done

# ── Check the arguments ─────────────────────────

if [[ -z $DISK || -z $NAME || -z $SSH_PORT || -z $OUT ]]; then
    usage >&2
    die "--disk, --name, --ssh-port and --out are required"
fi

# A hostname label, which also keeps it safe in file names and YAML
if [[ ! $NAME =~ ^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$ ]]; then
    die "--name must be a hostname of letters, digits and '-': '$NAME'"
fi
if [[ ! $GUEST_USER =~ ^[a-z_][a-z0-9_-]{0,31}$ ]]; then
    die "--user isn't a user name: '$GUEST_USER'"
fi

check_uint --ssh-port "$SSH_PORT" 1 65535
check_uint --cpus "$CPUS" 1 1024
check_uint --mem "$MEM" 256 16777216
check_uint --timeout "$TIMEOUT" 1 86400
check_uint --nvme "$NVME" 0 32

case $ACCEL in
    kvm) : "${CPU:=host}" ;;
    tcg) : "${CPU:=qemu64}" ;;
    *) die "--accel must be kvm or tcg: '$ACCEL'" ;;
esac
if [[ ! $CPU =~ ^[A-Za-z0-9][A-Za-z0-9_.-]*$ ]]; then
    die "--cpu isn't a CPU model: '$CPU'"
fi

DISK=$(qemu_path --disk "$DISK")
if [[ ! -f $DISK || ! -r $DISK ]]; then
    die "--disk isn't a readable file: $DISK"
fi

# The key goes here, so it must be ours alone, and not a link to
# somewhere that isn't
OUT=$(qemu_path --out "$OUT")
mkdir -p -- "$OUT"
if [[ -L $OUT || ! -d $OUT || ! -O $OUT ]]; then
    die "--out must be a directory owned by you: $OUT"
fi
chmod 700 -- "$OUT"

CONSOLE=$(qemu_path --console "${CONSOLE:-$OUT/console.log}")
if [[ -n $QMP ]]; then
    QMP=$(qemu_path --qmp "$QMP")
    clear_socket --qmp "$QMP"
fi
if [[ -n $QGA ]]; then
    QGA=$(qemu_path --qga "$QGA")
    clear_socket --qga "$QGA"
fi

if [[ -n $VFIO_USER ]]; then
    VFIO_USER=$(qemu_path --vfio-user "$VFIO_USER")
    # It goes into a JSON string
    if [[ $VFIO_USER == *[\"\\]* ]]; then
        die "--vfio-user can't contain a quote or backslash: $VFIO_USER"
    fi
    if [[ ! -S $VFIO_USER ]]; then
        die "--vfio-user isn't a socket: $VFIO_USER"
    fi
fi
if [[ ! $VFIO_USER_ID =~ ^[A-Za-z][A-Za-z0-9_.-]{0,63}$ ]]; then
    die "--vfio-user-id isn't a QEMU ID: '$VFIO_USER_ID'"
fi

if [[ -n $MCAST ]] &&
    [[ ! $MCAST =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}:[0-9]{1,5}$ ]]; then
    die "--mcast must be an IPv4 ADDR:PORT: '$MCAST'"
fi

for rule in "${HOSTFWDS[@]}"; do
    # A comma would start another -netdev option
    if [[ ! $rule =~ ^(tcp|udp)?:[^,[:space:]]*:[0-9]+-[^,[:space:]]*:[0-9]+$ ]]; then
        die "--hostfwd must be [tcp|udp]:[ADDR]:PORT-[ADDR]:PORT: '$rule'"
    fi
done

for i in "${!HOSTDEVS[@]}"; do
    bdf=${HOSTDEVS[i]}
    if [[ ! $bdf =~ ^([0-9a-fA-F]{4}:)?[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$ ]]; then
        die "--pci-hostdev isn't a PCI address: '$bdf'"
    fi
    if [[ $bdf != ????:* ]]; then
        bdf="0000:$bdf"
    fi
    bdf=${bdf,,}
    HOSTDEVS[i]=$bdf
    driver=$(readlink -- "/sys/bus/pci/devices/$bdf/driver" || true)
    if [[ ${driver##*/} != vfio-pci ]]; then
        die "--pci-hostdev $bdf isn't bound to vfio-pci"
    fi
    group=$(readlink -- "/sys/bus/pci/devices/$bdf/iommu_group" || true)
    if [[ -z $group || ! -r /dev/vfio/${group##*/} ||
        ! -w /dev/vfio/${group##*/} ]]; then
        die "--pci-hostdev $bdf: can't open its IOMMU group's" \
            "/dev/vfio/${group##*/}"
    fi
done

if ((NVME > 0)); then
    NVME_DIR=$(qemu_path --nvme-dir "${NVME_DIR:-$(dirname -- "$DISK")}")
    if [[ ! -d $NVME_DIR || ! -w $NVME_DIR ]]; then
        die "--nvme-dir isn't a writable directory: $NVME_DIR"
    fi
fi
case $NVME_AIO in
    "" | io_uring | native | threads) ;;
    *) die "--nvme-aio must be io_uring, native or threads: '$NVME_AIO'" ;;
esac

if ! command -v -- "$QEMU" >/dev/null; then
    die "QEMU not found: $QEMU"
fi
if ((NVME > 0)) && [[ -z $QEMU_IMG ]]; then
    QEMU_IMG=qemu-img
    if [[ $QEMU == */* && -x $(dirname -- "$QEMU")/qemu-img ]]; then
        QEMU_IMG=$(dirname -- "$QEMU")/qemu-img
    fi
fi
if ((NVME > 0)) && ! command -v -- "$QEMU_IMG" >/dev/null; then
    die "qemu-img not found: $QEMU_IMG"
fi
if ! command -v cloud-localds >/dev/null; then
    die "cloud-localds not found; install cloud-image-utils"
fi
for tool in ssh ssh-keygen setsid; do
    if ! command -v "$tool" >/dev/null; then
        die "$tool not found"
    fi
done

# ── The key and the seed ────────────────────────

KEY="$OUT/id_ed25519"
rm -f -- "$KEY" "$KEY.pub"
ssh-keygen -q -t ed25519 -N '' -C "$PROG $NAME" -f "$KEY" </dev/null

# Add the keys in a public key file to the user-data being written, each
# as a single-quoted YAML string.
#   $1  the file
add_keys() {
    local line found=false

    while IFS= read -r line || [[ -n $line ]]; do
        if [[ -z $line || $line == \#* ]]; then
            continue
        fi
        if [[ ! $line =~ ^(ssh-[a-z0-9-]+|ecdsa-sha2-[a-z0-9-]+|sk-[A-Za-z0-9@.-]+)\ [A-Za-z0-9+/]+=*(\ [^[:cntrl:]]*)?$ ]]; then
            die "$1 holds something other than SSH public keys"
        fi
        echo "      - '${line//\'/\'\'}'"
        found=true
    done <"$1"
    if ! $found; then
        die "$1 holds no SSH public key"
    fi
}

# The keys go to --user. cloud-init merges an entry named for the image's
# default user into that user's own settings, so for that user this changes
# nothing else; another account gets the keys and a locked password, and is
# created, without sudo, if it doesn't exist. "default" keeps the image's
# default user, which a users list without it would drop.
{
    echo '#cloud-config'
    echo "hostname: $NAME"
    echo 'users:'
    echo '  - default'
    echo "  - name: '$GUEST_USER'"
    echo '    ssh_authorized_keys:'
    add_keys "$KEY.pub"
    for pub in "${AUTHORIZE[@]}"; do
        if [[ ! -f $pub || ! -r $pub ]]; then
            die "--authorize isn't a readable file: $pub"
        fi
        add_keys "$pub"
    done
} >"$OUT/user-data"

INSTANCE_ID="$NAME-$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')"
printf 'instance-id: %s\nlocal-hostname: %s\n' "$INSTANCE_ID" "$NAME" \
    >"$OUT/meta-data"

# Only the results go to stdout
rm -f -- "$OUT/seed.img"
cloud-localds "$OUT/seed.img" "$OUT/user-data" "$OUT/meta-data" >&2

# ── The QEMU command line ───────────────────────

# Before the other devices, so none has taken its fixed slot
BRIDGE_ARGS=()
NVME_DEV_OPTS=""
if $MMIO_BRIDGE; then
    help=$("$QEMU" -device help 2>&1) || true
    if [[ $help != *'name "pci-mmio-bridge"'* ]]; then
        die "$QEMU has no pci-mmio-bridge device"
    fi
    BRIDGE_ARGS=(
        -device "pci-mmio-bridge,id=mmio-bridge,shadow-gpa=0x80000000,shadow-size=8192,poll-interval-ns=1000000,addr=8.0"
        -trace 'enable=pci_mmio_*'
    )
    # With the bridge, the NVMe devices take doorbell writes as plain
    # MMIO, not through an eventfd or shadow doorbells
    help=$("$QEMU" -device nvme,help 2>&1) || true
    if [[ $help == *ioeventfd=* ]]; then
        NVME_DEV_OPTS+=",ioeventfd=off"
    fi
    if [[ $help == *dbcs=* ]]; then
        NVME_DEV_OPTS+=",dbcs=off"
    fi
fi

# The first AIO mode QEMU can open an NVMe image with, as the images
# will be opened
probe_aio() {
    local probe aio

    probe=$(mktemp -- "$NVME_DIR/.aio-probe.XXXXXX")
    if ! "$QEMU_IMG" create -q -f qcow2 "$probe" 1M >&2; then
        rm -f -- "$probe"
        die "$QEMU_IMG couldn't make an image in $NVME_DIR"
    fi
    for aio in io_uring native; do
        if echo quit | timeout 10 "$QEMU" -machine none -display none \
            -monitor stdio \
            -drive "file=$probe,format=qcow2,if=none,id=probe,aio=$aio,cache.direct=on" \
            >/dev/null 2>&1; then
            break
        fi
        aio=threads
    done
    rm -f -- "$probe"
    echo "$aio"
}

NVME_ARGS=()
if ((NVME > 0)); then
    if [[ -z $NVME_AIO ]]; then
        NVME_AIO=$(probe_aio)
    fi
    for ((i = 1; i <= NVME; i++)); do
        img="$NVME_DIR/$NAME-nvme$i.qcow2"
        if [[ ! -e $img ]]; then
            "$QEMU_IMG" create -q -f qcow2 "$img" 1024G >&2
        fi
        NVME_ARGS+=(
            -drive "file=$img,format=qcow2,if=none,id=nvme-$i,aio=$NVME_AIO,cache.direct=on,discard=unmap,detect-zeroes=unmap"
            -device "nvme,serial=$NAME-nvme$i,id=nvme-$i-dev$NVME_DEV_OPTS"
            -device "nvme-ns,drive=nvme-$i,bus=nvme-$i-dev,nsid=1"
        )
    done
fi

# Each passed-through function, and the vfio-user device, on a root port
# of its own, so it can be hot-plugged and keeps PCIe features such as
# atomics
CHASSIS=0
HOSTDEV_ARGS=()
for i in "${!HOSTDEVS[@]}"; do
    CHASSIS=$((CHASSIS + 1))
    HOSTDEV_ARGS+=(
        -device "pcie-root-port,id=pcie.$((i + 1)),chassis=$CHASSIS"
        -device "vfio-pci,id=vfio-host$((i + 1)),bus=pcie.$((i + 1)),host=${HOSTDEVS[i]}"
    )
done

# ernicctl attach and hot-reload plug the device back into pcie-vfu.1
VFU_ARGS=()
if [[ -n $VFIO_USER ]]; then
    CHASSIS=$((CHASSIS + 1))
    VFU_ARGS=(
        -device "pcie-root-port,id=pcie-vfu.1,chassis=$CHASSIS"
        -device "{\"driver\":\"vfio-user-pci\",\"id\":\"$VFIO_USER_ID\",\"bus\":\"pcie-vfu.1\",\"socket\":{\"type\":\"unix\",\"path\":\"$VFIO_USER\"}}"
    )
fi

DISK_OPTS="file=$DISK,format=qcow2,if=virtio"
if $SHARED_BACKING; then
    DISK_OPTS+=",backing.file.locking=off"
fi

NETDEV="user,id=net0,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22"
for rule in "${HOSTFWDS[@]}"; do
    NETDEV+=",hostfwd=$rule"
done

# The MAC is derived from the SSH port, which is unique per guest
MCAST_ARGS=()
if [[ -n $MCAST ]]; then
    mac=$(printf '52:54:00:00:%02x:%02x' $((SSH_PORT / 256)) \
        $((SSH_PORT % 256)))
    MCAST_ARGS=(
        -netdev "socket,id=net1,mcast=$MCAST"
        -device "virtio-net-pci,netdev=net1,mac=$mac"
    )
fi

QGA_ARGS=()
if [[ -n $QGA ]]; then
    QGA_ARGS=(
        -chardev "socket,id=qga0,path=$QGA,server=on,wait=off"
        -device "virtio-serial-pci,id=virtio-serial0"
        -device "virtserialport,chardev=qga0,bus=virtio-serial0.0,name=org.qemu.guest_agent.0"
    )
fi

QMP_ARGS=()
if [[ -n $QMP ]]; then
    QMP_ARGS=(-qmp "unix:$QMP,server=on,wait=off")
fi

SNAPSHOT_ARGS=()
if $SNAPSHOT; then
    SNAPSHOT_ARGS=(-snapshot)
fi

# vfio-user needs guest memory the server can map, so it's a shared memfd.
#
# hpet=off: now and then a nested guest's kernel hangs for good just after
# setting up the HPET. See "boot hang" in scripts/qemu-vm-diagnose.sh.
CMD=(
    "$QEMU"
    -machine "q35,accel=$ACCEL,memory-backend=mem0,hpet=off"
    -cpu "$CPU"
    -smp "$CPUS"
    -m "${MEM}M"
    -object "memory-backend-memfd,id=mem0,share=on,size=${MEM}M"
    -display none
    -monitor none
    -serial "file:$CONSOLE"
    "${BRIDGE_ARGS[@]}"
    "${HOSTDEV_ARGS[@]}"
    "${NVME_ARGS[@]}"
    "${VFU_ARGS[@]}"
    -drive "$DISK_OPTS"
    -drive "file=$OUT/seed.img,format=raw,if=virtio,readonly=on"
    -netdev "$NETDEV"
    -device "virtio-net-pci,netdev=net0"
    "${MCAST_ARGS[@]}"
    "${QGA_ARGS[@]}"
    "${QMP_ARGS[@]}"
    "${SNAPSHOT_ARGS[@]}"
    "${EXTRA[@]}"
)

# ── Boot, and wait for SSH ──────────────────────

QEMU_PID=""
WAIT_PID=""
BOOTED=false

# Stop QEMU unless the guest came up, and whatever the wait was waiting on
stop_qemu() {
    trap '' INT TERM HUP
    if [[ -n $WAIT_PID ]]; then
        kill -TERM "$WAIT_PID" 2>/dev/null || true
    fi
    if $BOOTED || [[ -z $QEMU_PID ]]; then
        return
    fi
    kill -TERM "$QEMU_PID" 2>/dev/null || return 0
    for _ in {1..20}; do
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            return
        fi
        sleep 0.5
    done
    kill -KILL "$QEMU_PID" 2>/dev/null || true
}
trap stop_qemu EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

QEMU_LOG="$OUT/qemu.log"
printf '%q ' "${CMD[@]}" >"$QEMU_LOG"
printf '\n' >>"$QEMU_LOG"

# setsid takes QEMU out of the caller's session, so it outlives a terminal
# or a sudo pty that closes. With no job control here, the child isn't a
# process group leader, so setsid runs QEMU in place rather than forking,
# and $! is QEMU's PID.
setsid "${CMD[@]}" </dev/null >>"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

SSH=(
    ssh -F /dev/null -i "$KEY" -p "$SSH_PORT"
    -o BatchMode=yes -o IdentitiesOnly=yes
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
    -o LogLevel=ERROR -o ConnectTimeout=5
    "$GUEST_USER@127.0.0.1" true
)

echo "$PROG: started QEMU (PID $QEMU_PID); waiting up to ${TIMEOUT}s for" \
    "SSH on 127.0.0.1:$SSH_PORT" >&2
# Run a command in the background and wait for it, returning its status.
# A signal stops a script only once the command in the foreground has
# finished, but it stops a wait at once, so a caller stopping this, such
# as vm-stop cancelling a vm-launch, doesn't wait out a probe.
#   $@  the command
wait_for() {
    "$@" </dev/null &
    WAIT_PID=$!
    local status=0
    wait "$WAIT_PID" || status=$?
    WAIT_PID=""
    return "$status"
}

# Each probe gets at most what's left of the wait, so none ends after it
DEADLINE=$((SECONDS + TIMEOUT))
: >"$OUT/ssh.log"
while :; do
    left=$((DEADLINE - SECONDS))
    if ((left <= 0)); then
        echo "$PROG: the guest didn't accept SSH within ${TIMEOUT}s." \
            "ssh said:" >&2
        cat -- "$OUT/ssh.log" >&2
        echo "$PROG: $CONSOLE ends:" >&2
        tail -n 20 -- "$CONSOLE" >&2 || true
        exit 1
    fi
    if wait_for timeout "$((left < 60 ? left : 60))" "${SSH[@]}" \
        2>"$OUT/ssh.log"; then
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        status=0
        wait "$QEMU_PID" || status=$?
        echo "$PROG: QEMU exited with status $status. $QEMU_LOG ends:" >&2
        tail -n 20 -- "$QEMU_LOG" >&2
        exit 1
    fi
    wait_for sleep 2
done
BOOTED=true

# Only now, so a caller that has the results knows QEMU is running, even
# if this is stopped before it exits
echo "$PROG: the guest is up" >&2
printf 'pid=%s\nkey=%s\nuser=%s\nport=%s\n' \
    "$QEMU_PID" "$KEY" "$GUEST_USER" "$SSH_PORT"
