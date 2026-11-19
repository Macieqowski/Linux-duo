#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Install Debian + Plasma Mobile into the Surface Duo 1's userdata partition, from a Linux PC.
#
# What it touches on the phone: ONLY the userdata partition (formatted as ext4, Debian goes to /debian on it).
# It never writes boot_*, vbmeta_*, dtbo_*, the partition table or the lock state, and it never locks the bootloader.
# The kernel is always started from the PC with `fastboot boot` (RAM only): power off = the phone's own boot chain.
#
# usage: scripts/install-debian.sh <duo-debian.img> <duo-debian-rootfs.tar.zst> <duo-modules-*.tar.zst>
# needs: fastboot, ssh, ssh-keygen, python3, zstd; the phone with an UNLOCKED bootloader, in the bootloader menu
# (from off: hold volume-down, press power), connected over USB-C.
set -euo pipefail
IMG=$(readlink -f "$1"); ROOTFS=$(readlink -f "$2"); MODS=$(readlink -f "$3")
H=$(cd "$(dirname "$0")/.." && pwd)
for f in "$IMG" "$ROOTFS" "$MODS"; do [ -s "$f" ] || { echo "missing: $f"; exit 1; }; done
for t in fastboot ssh ssh-keygen python3 zstd; do command -v $t > /dev/null || { echo "missing tool: $t"; exit 1; }; done

KEY=${DUO_KEY:-$HOME/.ssh/duo_ed25519}
[ -f "$KEY" ] || ssh-keygen -q -t ed25519 -N '' -C duo-install -f "$KEY"
KIMG=$(dirname "$IMG")/duo-debian-key.img
python3 -I "$H/tools/linux/abl/add-ssh-key.py" "$IMG" "$KEY.pub" "$KIMG" > /dev/null
echo "boot image with your SSH key: $KIMG"

PHONE=root@169.254.66.1
SSH="ssh -i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5 -o ServerAliveInterval=10"

echo "1/4 starting Linux from RAM (nothing is written to the phone by this step)"
fastboot devices | grep -q . || { echo "no phone in fastboot: bootloader menu = from off, volume-down + power"; exit 1; }
fastboot getvar unlocked 2>&1 | grep -qi 'unlocked: *yes' || { echo "the bootloader is not unlocked (fastboot flashing unlock first)"; exit 1; }
fastboot --set-active=a
fastboot boot "$KIMG"

echo "2/4 waiting for the phone on USB networking (169.254.66.1)"
for i in $(seq 1 60); do $SSH $PHONE true 2>/dev/null && break; sleep 3; done
$SSH $PHONE true || { echo "no SSH answer from the phone. Does your PC have an address in 169.254.0.0/16 on the new USB network?"; exit 1; }

P=$($SSH $PHONE 'for d in /sys/class/block/sd*; do grep -q "^PARTNAME=userdata$" $d/uevent && echo ${d##*/}; done | head -1')
[ -n "$P" ] || { echo "userdata partition not found on the phone"; exit 1; }
SZ=$($SSH $PHONE "echo \$((\$(cat /sys/class/block/$P/size) / 2097152))")
echo
echo "3/4 userdata = /dev/$P (${SZ} GB). It will be FORMATTED (ext4) - everything on it is lost."
echo "    (Android's data is already gone after the bootloader unlock; Android itself stays on its own partitions.)"
read -r -p "    Type YES to continue: " ok; [ "$ok" = YES ] || { echo "stopped, nothing written"; exit 1; }
$SSH $PHONE "set -e; umount /dev/$P 2>/dev/null || true; mkfs.ext4 -q -F -L duo-userdata /dev/$P
  mkdir -p /mnt/u; mount /dev/$P /mnt/u; mkdir -p /mnt/u/debian"

echo "4/4 copying Debian (this takes a while)"
zstd -dc "$ROOTFS" | $SSH $PHONE "tar -x -C /mnt/u/debian"
zstd -dc "$MODS" | $SSH $PHONE "tar -x -C /mnt/u/debian"
$SSH $PHONE "set -e; chmod 1777 /mnt/u/debian/tmp; sync; umount /mnt/u; echo copied"

cat <<EOF

Done. Start Debian now and after every power-off (the phone in the bootloader menu, USB to this PC):

    fastboot --set-active=a && fastboot boot $KIMG

Plasma Mobile starts on both screens. SSH: ssh -i $KEY root@169.254.66.1 (the image keeps your key).
Holding power switches the phone off; the next normal start is Android's own boot chain again.
EOF
$SSH $PHONE 'devmem 0x0c264000 w 0' 2>/dev/null || true   # reset (PS_HOLD): `reboot` hangs this phone
