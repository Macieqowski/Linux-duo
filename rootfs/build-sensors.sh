#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Runs INSIDE the Debian chroot (rootfs/build-debian-rootfs.sh). Sensors for the Surface Duo 1 (accelerometer =
# auto-rotate, hinge angle/posture, light, proximity, compass), as linuxduobench did it on the phone:
#  1. hexagonrpcd 0.3.2 with the keep-serving patch (tools/linux/patches/hexagonrpcd-keep-serving.py) ->
#     /opt/duo/bin/hexagonrpcd, which duo-sensors prefers over Debian's
#  2. libssc + iio-sensor-proxy built with SSC support: trixie has neither, so Debian sid's own source packages are
#     rebuilt here (tools/linux/debian/duo-sensors-build-debian.sh did the same on the phone)
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get -q -y --no-install-recommends install build-essential curl python3 dpkg-dev devscripts equivs > /dev/null

T=/var/tmp/hx; rm -rf $T; mkdir -p $T; cd $T
curl -fsSL https://github.com/linux-msm/hexagonrpc/archive/refs/tags/v0.3.2.tar.gz | tar -xz
python3 -I /tmp/hexagonrpcd-keep-serving.py $T/hexagonrpc-0.3.2
cd hexagonrpc-0.3.2; grep -q "keep serving" hexagonrpcd/listener.c
mkdir -p /opt/duo/bin
cc -O2 -Wall -Wno-unused-parameter -Iinclude -o /opt/duo/bin/hexagonrpcd hexagonrpcd/*.c libhexagonrpc/*.c
echo "built /opt/duo/bin/hexagonrpcd"

echo 'deb-src http://deb.debian.org/debian sid main' > /etc/apt/sources.list.d/sid-src.list
apt-get -q update > /dev/null
B=/var/tmp/bp; rm -rf $B; mkdir -p $B; cd $B
apt-get -q source libssc iio-sensor-proxy > /dev/null
for p in libssc-* iio-sensor-proxy-*; do
  [ -d "$p" ] || continue
  (cd "$p" && mk-build-deps -i -r -t 'apt-get -y --no-install-recommends' debian/control > ../bd-$p.log 2>&1) || { tail -15 bd-$p.log; exit 1; }
  (cd "$p" && DEB_BUILD_OPTIONS=nocheck dpkg-buildpackage -b -us -uc -j"$(nproc)" > ../build-$p.log 2>&1) || { tail -30 build-$p.log; exit 1; }
  case $p in libssc-*) apt-get -y install ./libssc*.deb ./gir1.2-ssc*.deb > /dev/null 2>&1 || apt-get -y install ./libssc*.deb > /dev/null ;; esac
done
apt-get -y install ./iio-sensor-proxy_*.deb > /dev/null
apt-mark hold iio-sensor-proxy > /dev/null
# the hinge angle client for duo-posture (screens follow the hinge); libssc has no hinge class
if [ -f /tmp/duo-hinge.c ]; then
  mkdir -p /usr/libexec/duo
  cc -O2 /tmp/duo-hinge.c $(pkg-config --cflags --libs libssc) -o /usr/libexec/duo/duo-hinge && echo "sensors: duo-hinge built" ||
    echo "WARNING: duo-hinge build failed"
fi
rm -f /etc/apt/sources.list.d/sid-src.list
apt-get -y purge devscripts equivs > /dev/null 2>&1 || true
apt-get -y autoremove > /dev/null 2>&1 || true
rm -rf $T $B
echo "sensors: libssc + iio-sensor-proxy (SSC) installed"
