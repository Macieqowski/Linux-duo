#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the Surface Duo 1 kernel, its module package and the boot image that `fastboot boot` starts.
# Runs as root on an x86-64 Ubuntu/Debian machine (GitHub Actions: ubuntu-24.04) with:
#   gcc-aarch64-linux-gnu device-tree-compiler flex bison bc libssl-dev libelf-dev cpio zstd xz-utils python3 kmod curl git
# usage: sudo scripts/build-kernel-and-boot.sh <repo> <out-dir> [reference-boot.img|default]
#   env: LINUX_GIT (default /root/linux; created with a shallow fetch of $BASE when missing), BASE (default v7.3.5),
#        KEYS=<ssh public key file> to bake into the boot image (otherwise add one later: tools/linux/abl/add-ssh-key.py)
# Output in <out-dir>:
#   duo-debian.img                boot image: Debian from userdata:/debian, or the SSH installer shell when not there
#   duo-modules-<release>.tar.zst kernel modules (/usr/lib/modules + the Duo's /usr/lib/duo set), unpacked over the root
#   Image.gz, sm8150-microsoft-surface-duo.dtb, config-<release>, SHA256SUMS
set -euo pipefail
REPO=$(readlink -f "$1"); OUT=$(readlink -f "$2"); REF=${3:-default}
export LINUX_GIT=${LINUX_GIT:-/root/linux} BASE=${BASE:-v7.3.5}
L=$REPO/tools/linux; KO=/root/build-duo; W=/root/duo-work
mkdir -p "$OUT" "$W"
log() { echo "== $(date +%H:%M:%S) $*"; }

if ! git -C "$LINUX_GIT" rev-parse -q --verify "$BASE^{commit}" > /dev/null 2>&1; then
  log "fetch Linux $BASE"
  mkdir -p "$LINUX_GIT"; git -C "$LINUX_GIT" init -q
  git -C "$LINUX_GIT" fetch -q --depth 1 https://github.com/gregkh/linux tag "$BASE"
fi
# a checked-out tree too: make-initramfs.sh installs the UAPI headers from it
[ -f "$LINUX_GIT/Makefile" ] || git -C "$LINUX_GIT" -c advice.detachedHead=false checkout -q "$BASE"

log "kernel"
"$L/kernel/build.sh" "$REPO" /root/linux-duo-src "$KO"
KREL=$(cat $KO/include/config/kernel.release)

log "workbench initramfs (Alpine) - only its lib/duo module set is used"
"$L/make-initramfs.sh" "$REPO" "$W/base.cpio.gz"
KTREE=$KO "$L/initramfs-update-init.sh" "$REPO" "$W/base.cpio.gz" "$W/full.cpio.gz"

log "boot ramdisk (Debian)"
KEYS=${KEYS:-} MODE=boot "$L/debian/make-debian-rambench.sh" "$REPO" "$W/full.cpio.gz" "$W/ram.cpio.gz"

log "boot image"
CMD="rdinit=/init clk_ignore_unused pd_ignore_unused console=tty0 loglevel=4 panic=10 fbcon=font:TER16x32 fbcon=rotate:1 consoleblank=0 duo.root=userdata duo.rootdir=debian"
"$L/abl/make-abl-image.sh" "$KO/arch/arm64/boot/dts/qcom/sm8150-microsoft-surface-duo.dtb" "$KO/arch/arm64/boot/Image.gz" \
  "$W/ram.cpio.gz" "$REF" "$OUT/duo-debian.img" "$CMD"

log "module package"
M=$W/modules; rm -rf "$M"; mkdir -p "$M/usr/lib/duo/$KREL"
make -s -C "$KO" ARCH=arm64 INSTALL_MOD_PATH="$M/usr" INSTALL_MOD_STRIP=1 modules_install
rm -f "$M/usr/lib/modules/$KREL/build" "$M/usr/lib/modules/$KREL/source"
depmod -b "$M/usr" "$KREL"
# the flat module set the duo-* services insmod from /usr/lib/duo/<release> (the same set the RAM workbench carries)
(cd "$M/usr/lib/duo/$KREL" && zcat "$W/full.cpio.gz" | cpio -idm --quiet 'lib/duo/*' && cp -a lib/duo/. . && rm -rf lib)
[ -f "$KO/drivers/net/ipa/ipa.ko" ] && cp "$KO/drivers/net/ipa/ipa.ko" "$M/usr/lib/duo/$KREL/"
tar --numeric-owner -C "$M" -cf - usr | zstd -T0 -19 -q -o "$OUT/duo-modules-$KREL.tar.zst" -f

cp "$KO/arch/arm64/boot/Image.gz" "$KO/arch/arm64/boot/dts/qcom/sm8150-microsoft-surface-duo.dtb" "$OUT/"
cp "$KO/.config" "$OUT/config-$KREL"
(cd "$OUT" && sha256sum duo-debian.img "duo-modules-$KREL.tar.zst" Image.gz sm8150-microsoft-surface-duo.dtb > SHA256SUMS)
log "done: kernel $KREL"; ls -la "$OUT"
