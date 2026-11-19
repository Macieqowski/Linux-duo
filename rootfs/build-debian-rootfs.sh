#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build a Debian 13 "trixie" arm64 root file system with KDE Plasma Mobile for the Surface Duo 1.
# Runs as root on an x86-64 or arm64 Linux machine (GitHub Actions: ubuntu-24.04). On x86-64 it needs
# qemu-user-static + binfmt-support so the arm64 chroot can run.
#
# usage: sudo rootfs/build-debian-rootfs.sh <repo> <out.tar.zst> [kernel-build-dir]
#   kernel-build-dir: optional, the O= directory of tools/linux/kernel/build.sh; its modules are installed into the
#   rootfs. Without it the modules come separately (duo-modules-*.tar.zst, unpacked over the root at install time).
#   env: MIRROR (default http://deb.debian.org/debian), KEYRING (debootstrap's --keyring, e.g. Debian's archive keys
#        12 + 13 on an Ubuntu host whose keyring predates trixie), SKIP_SENSORS=1 (no libssc/iio-sensor-proxy backport),
#        SKIP_IPTSD=1 (no iptsd build), WORK (default /var/tmp/duo-rootfs)
#
# What it does:
#   1. debootstrap trixie (arm64)
#   2. installs Plasma Mobile + the Duo's userspace (ModemManager, rmtfs, tqftpserv, pd-mapper, hexagonrpcd, ...)
#      through rootfs/duo-debian-setup.sh (a trimmed copy of linuxduobench's tools/linux/debian/duo-debian-setup.sh)
#   3. installs the Duo system layer (tools/linux/system/install.sh: duo-hw, duo-modem, duo-sensors, duo-charger, ...)
#   4. builds iptsd (touch + pen heatmap daemon) and the sid libssc/iio-sensor-proxy backport (sensors) inside the chroot
#   5. installs the kernel modules
#   6. packs everything into a .tar.zst that goes to userdata:/debian on the phone (see docs/INSTALL.pl.md)
# No firmware is included: the Duo's own partitions supply it at boot (duo-hw, the initramfs).
set -euo pipefail
mkdir -p "$(dirname "$(readlink -m "$2")")"
REPO=$(readlink -f "$1"); OUT=$(readlink -m "$2"); KOUT=${3:+$(readlink -f "$3")}
MIRROR=${MIRROR:-http://deb.debian.org/debian}; W=${WORK:-/var/tmp/duo-rootfs}; R=$W/root
L=$REPO/tools/linux
[ -d "$L/system" ] && [ -n "$OUT" ] || { sed -n '2,21p' "$0"; exit 1; }
[ -z "$KOUT" ] || [ -f "$KOUT/include/config/kernel.release" ] || { echo "$KOUT is not a kernel build dir"; exit 1; }
log() { echo "== $(date +%H:%M:%S) $*"; }

cleanup() { for m in dev/pts dev proc sys run; do mountpoint -q "$R/$m" && umount -l "$R/$m" || true; done; }
trap cleanup EXIT
rm -rf "$W"; mkdir -p "$R"

log "debootstrap trixie arm64"
debootstrap --arch=arm64 --variant=minbase ${KEYRING:+--keyring="$KEYRING"} \
  --include=systemd,systemd-sysv,dbus,udev,apt,ca-certificates,locales,sudo,openssh-server,network-manager,kmod,zstd,less,nano \
  --components=main,contrib,non-free,non-free-firmware trixie "$R" "$MIRROR"

cat > "$R/etc/apt/sources.list" <<EOF
deb $MIRROR trixie main contrib non-free non-free-firmware
deb $MIRROR trixie-updates main contrib non-free non-free-firmware
deb $MIRROR trixie-backports main contrib non-free non-free-firmware
deb http://security.debian.org/debian-security trixie-security main contrib non-free non-free-firmware
EOF
# no daemons start inside the chroot
printf '#!/bin/sh\nexit 101\n' > "$R/usr/sbin/policy-rc.d"; chmod 755 "$R/usr/sbin/policy-rc.d"
for m in dev dev/pts proc sys run; do mkdir -p "$R/$m"; done
mount --bind /dev "$R/dev"; mount --bind /dev/pts "$R/dev/pts"
mount -t proc proc "$R/proc"; mount -t sysfs sys "$R/sys"; mount -t tmpfs tmpfs "$R/run"
cp /etc/resolv.conf "$R/etc/resolv.conf"
in_chroot() { chroot "$R" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LANG=C.UTF-8 \
  DEBIAN_FRONTEND=noninteractive "$@"; }

log "Plasma Mobile + Duo userspace"
cp "$REPO/rootfs/duo-debian-setup.sh" "$R/tmp/duo-debian-setup.sh"
cp "$L/debian/waydroid-nft.conf" "$R/tmp/waydroid-nft.conf"
cp "$L/modem/duo-gps.c" "$R/tmp/duo-gps.c"
in_chroot sh /tmp/duo-debian-setup.sh

log "Duo system layer"
sh "$L/system/install.sh" --init=systemd --root="$R" --desktop=plasma
# enable with the chroot's own systemctl (the host's may be older than trixie's units expect)
in_chroot systemctl enable duo-watchdog duo-bootanim duo-hw duo-bootanim-done duo-charger duo-modem duo-sensors \
  duo-bt-addr duo-mtp duo-autobrightness >/dev/null 2>&1 || true
[ -x "$R/usr/local/bin/duo-gps" ] && in_chroot systemctl enable duo-gps-assist >/dev/null 2>&1 || true
# the Duo's config files (audio UCM + tuning, udev/polkit rules, firewall, Chromium flags, hinge gap app):
# linuxduobench's own list, tools/linux/debian/duo-files.list (<source in tools/linux>:<destination in the root>)
grep -v '^#' "$L/debian/duo-files.list" | while IFS=: read -r src dst; do
  [ -n "$src" ] || continue
  case $src in *.sh) mode=755 ;; *) mode=644 ;; esac
  install -Dm$mode "$L/$src" "$R/$dst"
done

if [ "${SKIP_IPTSD:-0}" != 1 ]; then
  log "iptsd (touch and pen)"
  cp "$REPO/rootfs/build-iptsd.sh" "$R/tmp/"; in_chroot sh /tmp/build-iptsd.sh || echo "WARNING: iptsd build failed (touch falls back to single-touch)"
fi
if [ "${SKIP_SENSORS:-0}" != 1 ]; then
  log "libssc + iio-sensor-proxy backport (sensors, auto-rotate, hinge)"
  cp "$REPO/rootfs/build-sensors.sh" "$L/patches/hexagonrpcd-keep-serving.py" "$R/tmp/"; in_chroot sh /tmp/build-sensors.sh || echo "WARNING: sensor backport failed"
fi

if [ -n "$KOUT" ]; then
  KREL=$(cat "$KOUT/include/config/kernel.release"); log "kernel modules $KREL"
  make -s -C "$KOUT" ARCH=arm64 INSTALL_MOD_PATH="$R/usr" INSTALL_MOD_STRIP=1 modules_install
  rm -f "$R/usr/lib/modules/$KREL/build" "$R/usr/lib/modules/$KREL/source"
  in_chroot depmod -a "$KREL"
fi

log "clean up"
in_chroot apt-get clean
rm -rf "$R/var/lib/apt/lists/"* "$R/tmp/"* "$R/var/tmp/"* "$R/usr/sbin/policy-rc.d"
: > "$R/etc/machine-id"
echo "duo-debian" > "$R/etc/hostname"
cat > "$R/etc/fstab" <<'EOF'
# the root is mounted by the initramfs (duo.root=userdata duo.rootdir=debian)
tmpfs /tmp tmpfs defaults,nosuid,nodev 0 0
EOF
cleanup; trap - EXIT

log "pack $OUT"
tar --numeric-owner --xattrs --acls -C "$R" -cf - . | zstd -T0 -19 -o "$OUT" -f
ls -la "$OUT"; du -sh "$R"
