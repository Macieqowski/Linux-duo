#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Runs INSIDE the Debian chroot (rootfs/build-debian-rootfs.sh). iptsd (linux-surface) turns the Duo digitizer's
# capacitive heatmaps into multi-touch and pen input; duo-hw starts one per panel. Debian does not package it.
set -e
export DEBIAN_FRONTEND=noninteractive
V=${IPTSD_VERSION:-v3.1.1}
apt-get -q -y --no-install-recommends install build-essential curl ca-certificates meson ninja-build pkg-config cmake \
  libinih-dev libfmt-dev libspdlog-dev libeigen3-dev libcli11-dev libsdl2-dev libcairomm-1.0-dev libhidapi-dev \
  libmicrosoft-gsl-dev > /dev/null
T=/var/tmp/iptsd; rm -rf $T; mkdir -p $T; cd $T
curl -fsSL https://github.com/linux-surface/iptsd/archive/refs/tags/$V.tar.gz | tar -xz
cd iptsd-*
meson setup build --prefix=/usr --buildtype=release -Dservice_manager=systemd -Ddebug_tools=[] > ../setup.log 2>&1 \
  || meson setup build --prefix=/usr --buildtype=release > ../setup.log 2>&1 || { tail -30 ../setup.log; exit 1; }
ninja -C build > ../build.log 2>&1 || { tail -40 ../build.log; exit 1; }
ninja -C build install > /dev/null
# duo-hw starts iptsd itself (one per digitizer, its own presets): the generic udev/systemd hook stays unused
rm -f /usr/lib/udev/rules.d/*iptsd*.rules /etc/udev/rules.d/*iptsd*.rules 2>/dev/null || true
cd /; rm -rf $T
iptsd --version 2>/dev/null || true
echo "built iptsd $V"
