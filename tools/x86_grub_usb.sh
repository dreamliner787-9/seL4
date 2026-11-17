#!/usr/bin/env bash
#
# Copyright 2026, Bill Nguyen
#
# SPDX-License-Identifier: GPL-2.0-only
#
# Make a USB stick (or a raw disk image) that boots an x86 seL4 kernel and its
# initial task with GRUB, on both legacy BIOS and UEFI machines.
#
# Disk layout: MBR partition table, one bootable FAT partition at 1 MiB.
#   BIOS: GRUB's boot.img in the MBR, core.img in the gap before the partition
#   UEFI: GRUB as /EFI/BOOT/BOOTX64.EFI, the default path for removable media
#   both: /boot/grub/grub.cfg, /boot/sel4/kernel.elf, /boot/sel4/initial_task.elf
#
# Nothing is mounted: the file system is created and filled by dosfstools and
# mtools directly on the target, so the only thing that needs root is write
# access to the block device. All GRUB modules are built into the images, the
# GRUB installation of the host is not touched.

set -euo pipefail

PROG=$(basename "$0")

SECTOR=512
PART_START=2048 # sectors, 1 MiB. Also the space available for GRUB's core.img
MBR_MAX_SECTORS=4294967295

TIMEOUT=3
CMDLINE=""
GFXPAYLOAD=""
SERIAL=1
IMAGE_SIZE=""
ASSUME_YES=0
FORCE=0

usage() {
    cat <<EOF
Usage: $PROG [options] <kernel.elf> <initial-task.elf> <target>

Write a GRUB boot disk for an x86 seL4 system to <target>, which boots on both
legacy BIOS and UEFI machines. <target> is either

  - a whole block device, e.g. /dev/sdb or /dev/disk/by-id/usb-...
    ALL DATA ON IT IS DESTROYED. Needs write access, i.e. usually sudo.
  - a file, which is created/overwritten as a raw disk image, e.g. for
    testing with: qemu-system-x86_64 -drive format=raw,file=<target> ...

Options:
  -c, --cmdline STR      seL4 kernel command line, e.g. "console_port=0x2f8 disable_fbcon"
  -t, --timeout SECS     GRUB menu timeout (default: $TIMEOUT)
  -g, --gfxpayload MODE  GRUB 'gfxpayload', e.g. "keep" or "1024x768x32"
                         (default: unset, the kernel's multiboot header decides)
      --no-serial        do not also show the GRUB menu on the first serial port
  -s, --size SIZE        size of the image when <target> is a file, e.g. 256M
                         (default: big enough for the ELF files, at least 64M)
  -y, --yes              do not ask before erasing a block device
  -f, --force            also accept block devices that do not look like a
                         USB / removable disk
  -h, --help             show this help

Environment:
  GRUB_LIB   directory containing GRUB's i386-pc and x86_64-efi platform
             directories (default: auto-detect, e.g. /usr/lib/grub)
EOF
}

die() {
    echo "$PROG: error: $*" >&2
    exit 1
}

warn() {
    echo "$PROG: warning: $*" >&2
}

info() {
    echo "==> $*"
}

need() {
    for cmd in "$@"; do
        command -v "$cmd" >/dev/null 2>&1 || die "'$cmd' not found, please install it"
    done
}

is_elf() {
    [[ "$(head -c 4 -- "$1" | od -An -tx1 | tr -d ' \n')" == "7f454c46" ]]
}

# ------------------------------------------------------------------------------
# Arguments
# ------------------------------------------------------------------------------

POSITIONAL=()
while (($#)); do
    case $1 in
    -c | --cmdline)
        CMDLINE=${2?"$1 needs an argument"}
        shift 2
        ;;
    -t | --timeout)
        TIMEOUT=${2?"$1 needs an argument"}
        shift 2
        ;;
    -g | --gfxpayload)
        GFXPAYLOAD=${2?"$1 needs an argument"}
        shift 2
        ;;
    --no-serial)
        SERIAL=0
        shift
        ;;
    -s | --size)
        IMAGE_SIZE=${2?"$1 needs an argument"}
        shift 2
        ;;
    -y | --yes)
        ASSUME_YES=1
        shift
        ;;
    -f | --force)
        FORCE=1
        shift
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    --)
        shift
        POSITIONAL+=("$@")
        break
        ;;
    -*)
        die "unknown option '$1' (see --help)"
        ;;
    *)
        POSITIONAL+=("$1")
        shift
        ;;
    esac
done

if ((${#POSITIONAL[@]} != 3)); then
    usage >&2
    exit 1
fi

KERNEL=${POSITIONAL[0]}
INITIAL_TASK=${POSITIONAL[1]}
TARGET_ARG=${POSITIONAL[2]}

TIMEOUT_RE='^[0-9]+$'
# The command line ends up unquoted in grub.cfg, keep it to harmless characters
CMDLINE_RE='^[A-Za-z0-9_=,.:/+ -]*$'
GFXPAYLOAD_RE='^[A-Za-z0-9_,;]*$'
[[ $TIMEOUT =~ $TIMEOUT_RE ]] || die "invalid timeout '$TIMEOUT'"
[[ $CMDLINE =~ $CMDLINE_RE ]] || die "unsupported character in kernel command line '$CMDLINE'"
[[ $GFXPAYLOAD =~ $GFXPAYLOAD_RE ]] || die "invalid gfxpayload '$GFXPAYLOAD'"

for f in "$KERNEL" "$INITIAL_TASK"; do
    [[ -f $f ]] || die "'$f' does not exist"
    is_elf "$f" || die "'$f' is not an ELF file"
done

need sfdisk mkfs.fat mmd mcopy mdir od numfmt dd truncate

# ------------------------------------------------------------------------------
# Target
# ------------------------------------------------------------------------------

TARGET=$(realpath -m -- "$TARGET_ARG")

if [[ -b $TARGET ]]; then
    IS_DEVICE=1
elif [[ $TARGET == /dev/* && ! -f $TARGET ]]; then
    # never create a file in /dev because of a typo or a stick that is not plugged in
    die "'$TARGET_ARG' is not a block device"
elif [[ -e $TARGET && ! -f $TARGET ]]; then
    die "'$TARGET_ARG' is neither a block device nor a regular file"
else
    IS_DEVICE=0
fi

if ((IS_DEVICE)); then
    need lsblk blockdev wipefs

    DEV_TYPE=$(lsblk -dnro TYPE -- "$TARGET")
    case $DEV_TYPE in
    disk | loop) ;;
    part) die "$TARGET is a partition, give the whole disk (e.g. /dev/sdX, not /dev/sdX1)" ;;
    *) die "$TARGET is a '$DEV_TYPE' device, expected a whole disk" ;;
    esac

    # Neither the disk nor any of its partitions may be in use. Collect the
    # output first, piping lsblk into 'grep -q' does not mix with pipefail.
    MOUNTS=$(lsblk -nro MOUNTPOINT -- "$TARGET")
    if [[ -n ${MOUNTS//[[:space:]]/} ]]; then
        lsblk -o NAME,SIZE,MOUNTPOINT -- "$TARGET" >&2
        die "$TARGET (or a partition on it) is mounted or used as swap, unmount it first"
    fi
    HOLDERS=$(lsblk -nro TYPE -- "$TARGET" | tail -n +2)
    HOLDERS=${HOLDERS//part/}
    if [[ -n ${HOLDERS//[[:space:]]/} ]]; then
        lsblk -o NAME,SIZE,TYPE -- "$TARGET" >&2
        die "$TARGET is in use (LVM, dm-crypt, RAID, ...)"
    fi

    read -r DEV_RM DEV_HOTPLUG DEV_TRAN < <(lsblk -dnro RM,HOTPLUG,TRAN -- "$TARGET") || true
    if [[ $DEV_TYPE != loop && ${DEV_RM:-0} != 1 && ${DEV_HOTPLUG:-0} != 1 && ${DEV_TRAN:-} != usb ]] && ((!FORCE)); then
        lsblk -do NAME,SIZE,TRAN,RM,HOTPLUG,MODEL -- "$TARGET" >&2
        die "$TARGET does not look like a USB or removable disk, use --force if you really mean it"
    fi

    [[ -w $TARGET ]] || die "no write access to $TARGET, try again with sudo"

    LOGICAL_SECTOR=$(blockdev --getss "$TARGET")
    ((LOGICAL_SECTOR == SECTOR)) || die "$TARGET has $LOGICAL_SECTOR byte sectors, only $SECTOR is supported"
    TOTAL_BYTES=$(blockdev --getsize64 "$TARGET")
else
    if [[ -n $IMAGE_SIZE ]]; then
        TOTAL_BYTES=$(numfmt --from=iec "$IMAGE_SIZE") || die "invalid size '$IMAGE_SIZE'"
    else
        # payload plus room for GRUB and the file system, at least 64 MiB
        PAYLOAD=$(($(stat -c %s -- "$KERNEL") + $(stat -c %s -- "$INITIAL_TASK")))
        TOTAL_BYTES=$(((PAYLOAD / 1048576 + 32) * 1048576))
        ((TOTAL_BYTES >= 67108864)) || TOTAL_BYTES=67108864
    fi
fi

TOTAL_SECTORS=$((TOTAL_BYTES / SECTOR))
PART_SECTORS=$((TOTAL_SECTORS - PART_START))
# MBR partitions are limited to 2 TiB, keep the size a multiple of 1 MiB
((PART_SECTORS <= MBR_MAX_SECTORS - PART_START)) || PART_SECTORS=$((MBR_MAX_SECTORS - PART_START))
PART_SECTORS=$((PART_SECTORS / 2048 * 2048))
((PART_SECTORS >= 32768)) || die "target is too small ($TOTAL_BYTES bytes)"

PAYLOAD=$(($(stat -c %s -- "$KERNEL") + $(stat -c %s -- "$INITIAL_TASK")))
((PAYLOAD + 8388608 < PART_SECTORS * SECTOR)) || die "the ELF files do not fit on the target"

# ------------------------------------------------------------------------------
# GRUB images, built in a temporary directory before the target is touched
# ------------------------------------------------------------------------------

MKIMAGE=$(command -v grub-mkimage || command -v grub2-mkimage || true)
[[ -n $MKIMAGE ]] || die "grub-mkimage not found. Install GRUB's tools and platform files, e.g.
    Arch:          pacman -S grub
    Debian/Ubuntu: apt install grub-common grub-pc-bin grub-efi-amd64-bin
    Fedora:        dnf install grub2-tools grub2-pc-modules grub2-efi-x64-modules
(installing the package does not change how your machine boots)"

PC_DIR=""
EFI_DIR=""
for d in ${GRUB_LIB:+"$GRUB_LIB"} /usr/lib/grub /usr/lib/grub2 /usr/share/grub /usr/share/grub2 /usr/local/lib/grub; do
    if [[ -z $PC_DIR && -f $d/i386-pc/boot.img && -f $d/i386-pc/kernel.img ]]; then
        PC_DIR=$d/i386-pc
    fi
    if [[ -z $EFI_DIR && -f $d/x86_64-efi/kernel.img ]]; then
        EFI_DIR=$d/x86_64-efi
    fi
done
[[ -n $PC_DIR || -n $EFI_DIR ]] || die "found neither GRUB's i386-pc nor its x86_64-efi platform files, set GRUB_LIB"
[[ -n $PC_DIR ]] || warn "GRUB i386-pc files not found, the disk will NOT boot on legacy BIOS"
[[ -n $EFI_DIR ]] || warn "GRUB x86_64-efi files not found, the disk will NOT boot on UEFI"

TMP=$(mktemp -d)
trap 'rm -rf -- "$TMP"' EXIT

VOLUME_ID=$(od -An -N4 -tx4 /dev/urandom | tr -d ' \n')
DISK_ID=$(od -An -N4 -tx4 /dev/urandom | tr -d ' \n')
FS_UUID="${VOLUME_ID:0:4}-${VOLUME_ID:4:4}"

# Built into the images: find our partition, then normal mode reads grub.cfg
cat >"$TMP/early.cfg" <<EOF
search --no-floppy --fs-uuid --set=root $FS_UUID
set prefix=(\$root)/boot/grub
EOF

COMMON_MODULES="normal configfile search search_fs_uuid part_msdos part_gpt fat
    multiboot multiboot2 all_video serial terminal echo test ls cat help sleep
    halt reboot minicmd"

# Keep only the modules this GRUB version has, all of the above are optional
# extras except those needed to find and boot the system.
modules_for() {
    local dir=$1 m list=""
    shift
    for m in "$@"; do
        if [[ -f $dir/$m.mod ]]; then
            list+=" $m"
        else
            case $m in
            normal | search | search_fs_uuid | part_msdos | fat | multiboot | multiboot2 | biosdisk)
                die "GRUB module '$m' not found in $dir"
                ;;
            esac
        fi
    done
    echo "$list"
}

if [[ -n $PC_DIR ]]; then
    info "Building GRUB for BIOS from $PC_DIR"
    # shellcheck disable=SC2046,SC2086
    "$MKIMAGE" -O i386-pc -d "$PC_DIR" -o "$TMP/core.img" -c "$TMP/early.cfg" -p /boot/grub \
        $(modules_for "$PC_DIR" biosdisk $COMMON_MODULES)
    CORE_SIZE=$(stat -c %s "$TMP/core.img")
    ((CORE_SIZE <= (PART_START - 1) * SECTOR)) || die "core.img is too big ($CORE_SIZE bytes)"

    # boot.img goes into the MBR boot code area. Like grub-bios-setup does for
    # disks, nop out the jump over the check that forces a sane BIOS drive
    # number, some BIOSes boot USB sticks with a floppy drive number.
    cp "$PC_DIR/boot.img" "$TMP/boot.img"
    if [[ "$(od -An -tx1 -j $((0x66)) -N1 "$TMP/boot.img" | tr -d ' \n')" == "eb" ]]; then
        printf '\x90\x90' | dd of="$TMP/boot.img" bs=1 seek=$((0x66)) conv=notrunc status=none
    else
        warn "unexpected boot.img layout, not patching the boot drive check"
    fi
fi

if [[ -n $EFI_DIR ]]; then
    info "Building GRUB for UEFI from $EFI_DIR"
    # shellcheck disable=SC2046,SC2086
    "$MKIMAGE" -O x86_64-efi -d "$EFI_DIR" -o "$TMP/BOOTX64.EFI" -c "$TMP/early.cfg" -p /boot/grub \
        $(modules_for "$EFI_DIR" $COMMON_MODULES)
fi

{
    echo "# Generated by $PROG"
    echo "set timeout=$TIMEOUT"
    echo "set default=0"
    if ((SERIAL)); then
        cat <<'EOF'

# also show the menu on the first serial port, if there is one
if serial --unit=0 --speed=115200; then
    terminal_input --append serial
    terminal_output --append serial
fi
EOF
    fi
    if [[ -n $GFXPAYLOAD ]]; then
        echo
        echo "set gfxpayload=\"$GFXPAYLOAD\""
    fi
    cat <<EOF

# multiboot2 works on BIOS and UEFI. With multiboot 1 seL4 has to find the ACPI
# tables in the BIOS area by itself, so that one is for legacy BIOS only.
menuentry 'seL4 (multiboot2)' --id sel4-mb2 {
    echo 'Loading seL4 kernel ...'
    multiboot2 /boot/sel4/kernel.elf $CMDLINE
    echo 'Loading initial task ...'
    module2 /boot/sel4/initial_task.elf initial_task
    boot
}

menuentry 'seL4 (multiboot)' --id sel4-mb1 {
    echo 'Loading seL4 kernel ...'
    multiboot /boot/sel4/kernel.elf $CMDLINE
    echo 'Loading initial task ...'
    module /boot/sel4/initial_task.elf initial_task
    boot
}
EOF
} >"$TMP/grub.cfg"

# ------------------------------------------------------------------------------
# Confirmation
# ------------------------------------------------------------------------------

if ((IS_DEVICE)); then
    echo
    lsblk -o NAME,SIZE,TYPE,TRAN,MODEL,LABEL,FSTYPE,MOUNTPOINT -- "$TARGET"
    echo
    if ((!ASSUME_YES)); then
        [[ -t 0 ]] || die "refusing to erase $TARGET without confirmation, use --yes"
        NAME=$(basename "$TARGET")
        read -r -p "ALL DATA ON $TARGET WILL BE DESTROYED. Type '$NAME' to continue: " ANSWER
        [[ $ANSWER == "$NAME" ]] || die "aborted, nothing was written"
    fi
fi

# ------------------------------------------------------------------------------
# Write
# ------------------------------------------------------------------------------

if ((IS_DEVICE)); then
    info "Erasing old partition table and file system signatures on $TARGET"
    wipefs --all --quiet "$TARGET"
else
    info "Creating $(numfmt --to=iec "$TOTAL_BYTES") disk image $TARGET"
    truncate -s 0 -- "$TARGET"
    truncate -s "$TOTAL_BYTES" -- "$TARGET"
fi
# clear the MBR and the gap that will hold GRUB's core.img
dd if=/dev/zero of="$TARGET" bs=1M count=1 conv=notrunc status=none

info "Partitioning"
printf 'label: dos\nlabel-id: 0x%s\nunit: sectors\n\nstart=%d, size=%d, type=c, bootable\n' \
    "$DISK_ID" "$PART_START" "$PART_SECTORS" |
    sfdisk --quiet --no-reread --no-tell-kernel -- "$TARGET"

info "Creating FAT file system (UUID $FS_UUID)"
# dosfstools >= 4.2 may put a fake MBR into the boot sector when formatting a
# whole disk, we have a real one
MKFS_EXTRA=()
MKFS_HELP=$(mkfs.fat --help 2>&1 || true)
if [[ $MKFS_HELP == *--mbr* ]]; then
    MKFS_EXTRA+=(--mbr=n)
fi
mkfs.fat -I "${MKFS_EXTRA[@]}" --offset "$PART_START" -h "$PART_START" -i "$VOLUME_ID" -n SEL4BOOT \
    "$TARGET" $((PART_SECTORS * SECTOR / 1024)) >/dev/null

if [[ -n $PC_DIR ]]; then
    info "Installing GRUB for BIOS"
    # only the 440 bytes of boot code, the disk id and partition table stay
    dd if="$TMP/boot.img" of="$TARGET" bs=440 count=1 conv=notrunc status=none
    dd if="$TMP/core.img" of="$TARGET" bs=$SECTOR seek=1 conv=notrunc status=none
fi

info "Copying files"
export MTOOLS_SKIP_CHECK=1
FAT="$TARGET@@$((PART_START * SECTOR))"
mmd -i "$FAT" ::/boot ::/boot/grub ::/boot/sel4
mcopy -o -i "$FAT" "$TMP/grub.cfg" ::/boot/grub/grub.cfg
mcopy -o -i "$FAT" "$KERNEL" ::/boot/sel4/kernel.elf
mcopy -o -i "$FAT" "$INITIAL_TASK" ::/boot/sel4/initial_task.elf
if [[ -n $EFI_DIR ]]; then
    mmd -i "$FAT" ::/EFI ::/EFI/BOOT
    mcopy -o -i "$FAT" "$TMP/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
fi

sync
if ((IS_DEVICE)); then
    blockdev --flushbufs "$TARGET" || true
    # let the OS see the new partition, not fatal if it cannot
    blockdev --rereadpt "$TARGET" 2>/dev/null || true
fi

echo
mdir -i "$FAT" -/ -b ::/
echo
if [[ -n $PC_DIR && -n $EFI_DIR ]]; then
    FIRMWARE="BIOS and UEFI"
elif [[ -n $PC_DIR ]]; then
    FIRMWARE="BIOS"
else
    FIRMWARE="UEFI"
fi
info "Done: $TARGET boots seL4 with GRUB on $FIRMWARE machines"
if ((!IS_DEVICE)); then
    cat <<EOF
    Try it with:
      qemu-system-x86_64 -machine q35 -accel kvm -cpu host -m 2G -serial mon:stdio -drive format=raw,file=$TARGET
    Add '-bios /usr/share/edk2/x64/OVMF.4m.fd' (path depends on the distro) to boot it with UEFI.
EOF
fi
