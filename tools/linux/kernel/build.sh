#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the Surface Duo kernel: stable v7.3.5 + the patch series in tools/linux/kernel/series (linuxduobench's v6.19
# series rebased onto v7.3; patches already upstream were dropped, see the last patch of the series), configured with
# `make defconfig duo.config`. Nothing else touches the tree: no patchers, no file drops.
# Based on linuxduobench's build.sh (GPL-2.0-or-later).
# usage (WSL root): build.sh <repo> [src=/root/linux-duo-src] [out=/root/build-duo]
#   needs a Linux git repo with the $BASE tag (default v7.3.5) at /root/linux (or LINUX_GIT=<repo>); src becomes a
#   worktree of it. Get one quickly with: git init linux && git -C linux fetch --depth 1 https://github.com/gregkh/linux tag v7.3.5
#   out: arch/arm64/boot/{Image.gz,vmlinuz.efi} (vmlinuz.efi = the ESP's default kernel), dts/qcom/
#   sm8150-microsoft-surface-duo.dtb, all modules; then initramfs-update-init.sh / pack-test-modules.sh take them.
set -e
REPO=$1; SRC=${2:-/root/linux-duo-src}; OUT=${3:-/root/build-duo}; G=${LINUX_GIT:-/root/linux}; BASE=${BASE:-v7.3.5}; CROSS=${CROSS_COMPILE:-aarch64-linux-gnu-}
S=$REPO/tools/linux/kernel/series
[ -n "$(ls $S/*.patch 2>/dev/null)" ] || { echo "no series in $S"; exit 1; }
git -C $G rev-parse -q --verify $BASE^{commit} >/dev/null || { echo "$G has no $BASE tag"; exit 1; }
if [ -d $SRC ]; then git -C $SRC am --abort 2>/dev/null || true; git -C $SRC checkout -q --detach $BASE; git -C $SRC reset -q --hard $BASE; git -C $SRC clean -qfdx
else git -C $G worktree add -q --detach $SRC $BASE; fi
cd $SRC
tr -d '\r' < /dev/null > /dev/null
for p in $S/*.patch; do tr -d '\r' < $p > /tmp/duo-series-$$.patch
  git -c user.name=duo-workbench -c user.email=duo@localhost am -q --keep-cr /tmp/duo-series-$$.patch || { git am --abort; echo "series FAILED at $(basename $p)"; exit 1; }; done
rm -f /tmp/duo-series-$$.patch
echo "series applied: $(ls $S/*.patch | wc -l) patches, HEAD $(git log -1 --format='%h %s' | cut -c1-80)"
M="make ARCH=arm64 CROSS_COMPILE=$CROSS O=$OUT"
mkdir -p $OUT; $M defconfig duo.config > /tmp/duo-config.log 2>&1 || { tail /tmp/duo-config.log; exit 1; }
$M -j$(nproc) Image.gz vmlinuz.efi modules qcom/sm8150-microsoft-surface-duo.dtb > /tmp/duo-build.log 2>&1 || {
  grep -E ' error|Error [0-9]' /tmp/duo-build.log | tail -20; echo KERNEL_BUILD_FAILED; exit 1; }
grep -E "warning:" /tmp/duo-build.log | grep -E "panel-microsoft|spi-hid-duo|venus|camss|qcom_smbx|dsi_host|qmp-combo|ufs-qcom|dp_display" | head
echo "release $(cat $OUT/include/config/kernel.release)"
md5sum $OUT/arch/arm64/boot/vmlinuz.efi $OUT/arch/arm64/boot/dts/qcom/sm8150-microsoft-surface-duo.dtb
echo KERNEL_BUILD_DONE
