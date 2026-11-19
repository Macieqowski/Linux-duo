#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# duo-debian-setup - runs INSIDE the Debian trixie arm64 tree (chroot) and makes it a Surface Duo 1 phone system with
# KDE Plasma Mobile. Trimmed from linuxduobench's tools/linux/debian/duo-debian-setup.sh: the same hardware layer and
# workarounds, without VS Code, OpenSnitch, Hingeboard and Porchlight; Plasma Mobile's phone apps added.
# Called by rootfs/build-debian-rootfs.sh. Optional inputs in /tmp: waydroid-nft.conf, duo-gps.c.
set -e
export PATH=/usr/sbin:/usr/bin:/sbin:/bin DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
apt-get -q update

# the Duo's hardware userspace (must all install)
apt-get -q -y --no-install-recommends install sddm hexagonrpcd iio-sensor-proxy modemmanager rmtfs tqftpserv \
  protection-domain-mapper qrtr-tools umtp-responder avahi-daemon alsa-ucm-conf alsa-utils python3-dbus python3-gi \
  netcat-openbsd psmisc libqmi-utils iproute2 qbootctl gdisk accountsservice \
  wpasupplicant bluez systemd-timesyncd systemd-coredump nftables \
  pipewire pipewire-pulse pipewire-alsa wireplumber libspa-0.2-bluetooth \
  bluedevil kde-cli-tools kio-extras ksystemstats systemsettings kscreen powerdevil plasma-nm \
  qtwayland5 qt6-wayland fonts-noto-core fonts-noto-color-emoji

# Plasma Mobile itself (must install)
apt-get -q -y install plasma-mobile

# phone apps: each on its own, so one name missing from trixie does not stop the build
for p in plasma-dialer spacebar plasma-settings maliit-keyboard angelfish kclock kweather kalk koko \
         discover plasma-discover-backend-flatpak flatpak chromium gnome-snapshot hfd-service plasma-mobile-tweaks; do
  apt-get -q -y install "$p" > /tmp/apt-$p.log 2>&1 && echo "installed $p" || echo "not available: $p"
done

# Mesa from backports: newer freedreno/turnip for the Adreno 640. Last, after everything from trixie: backports' Mesa
# pulls a newer libelf, and trixie packages installed after it (bluez -> libdw) would then not resolve.
apt-get -q -y -t trixie-backports install libegl-mesa0 libgbm1 libgl1-mesa-dri libglx-mesa0 mesa-libgallium \
  mesa-vulkan-drivers || echo "WARNING: Mesa from backports failed, keeping trixie's"

# the phone's user: auto-login, no password (set one with `passwd` on the phone before enabling SSH passwords)
id user > /dev/null 2>&1 || useradd -m -u 10000 -s /bin/bash -G audio,video,input,render,plugdev,netdev,dialout,sudo user
passwd -d user > /dev/null
S=$(ls /usr/share/wayland-sessions | grep -m1 -i plasma-mobile || ls /usr/share/wayland-sessions | grep -m1 -i plasma)
mkdir -p /etc/sddm.conf.d
printf '[Autologin]\nUser=user\nSession=%s\n\n[General]\nDisplayServer=wayland\n' "$S" > /etc/sddm.conf.d/10-duo-autologin.conf
echo duo-debian > /etc/hostname

# Bluetooth firmware: ONLY qca/crbtfw21.tlv + crnv21.bin out of firmware-atheros - the whole package also brings the
# generic ath10k WCN3990 board-2.bin that crashes the Duo's Wi-Fi (WCSS), so it is never installed (linuxduobench)
if [ ! -f /lib/firmware/qca/crbtfw21.tlv ]; then
  (cd /tmp && rm -rf fwa && mkdir fwa && cd fwa && apt-get -q download firmware-atheros > /dev/null 2>&1 &&
   dpkg-deb -x firmware-atheros_*.deb x && mkdir -p /lib/firmware/qca &&
   cp x/usr/lib/firmware/qca/crbtfw21.tlv* x/usr/lib/firmware/qca/crnv21.bin* /lib/firmware/qca/ &&
   echo "bluetooth firmware: crbtfw21 + crnv21")
  rm -rf /tmp/fwa
fi

# NO system suspend yet: Plasma's PowerDevil suspended the phone on idle (s2idle) and nothing woke it again
# (linuxduobench 2026-11-08). The sleep targets stay masked until duo-sleep is ported to Debian.
for t in sleep.target suspend.target hibernate.target hybrid-sleep.target suspend-then-hibernate.target; do
  ln -sf /dev/null /etc/systemd/system/$t; done
mkdir -p /home/user/.config
printf '[Daemon]\nAutolock=false\nLockOnResume=false\n' > /home/user/.config/kscreenlockerrc
printf '[Wallet]\nEnabled=false\nFirst Use=false\n' > /home/user/.config/kwalletrc
MRC=/home/user/.config/plasmamobilerc; touch $MRC
grep -q '^\[InitialStart\]' $MRC || printf '\n[InitialStart]\nwizardRun=true\n' >> $MRC

# touch under KWin: the digitizer (landscape) -> portrait panel, one digitizer per panel (measured by linuxduobench)
cat > /home/user/.config/kcminputrc << 'KCM'
[Libinput][1118][3073][IPTSD Virtual Touchscreen 045E:0C01]
CalibrationMatrix=0,-1,1,0,-1,0,1,0,0,0,1,0,0,0,0,1
OutputName=DSI-2

[Libinput][1118][3074][IPTSD Virtual Touchscreen 045E:0C02]
CalibrationMatrix=0,-1,1,0,-1,0,1,0,0,0,1,0,0,0,0,1
OutputName=DSI-1
KCM
chown -R user:user /home/user/.config

# the initramfs keeps the whole userdata partition at /mnt/duo-part (duo.rootdir=debian); the duo services look for
# extra tools under /mnt/linux
mkdir -p /mnt/linux
grep -q ' /mnt/linux ' /etc/fstab 2>/dev/null || echo '/mnt/duo-part /mnt/linux none bind 0 0' >> /etc/fstab
mkdir -p /etc/NetworkManager/conf.d
printf '[device-usb0]\nmatch-device=interface-name:usb0\nmanaged=false\n' > /etc/NetworkManager/conf.d/50-duo-usb0.conf

# duo-gps (QMI LOC client) built for glibc
if [ -s /tmp/duo-gps.c ]; then
  apt-get -q -y --no-install-recommends install gcc libc6-dev pkg-config libqmi-glib-dev > /tmp/apt-gps.log 2>&1 &&
    gcc -O2 -o /usr/local/bin/duo-gps /tmp/duo-gps.c $(pkg-config --cflags --libs qmi-glib) &&
    echo "built /usr/local/bin/duo-gps"
  rm -f /tmp/duo-gps.c
fi

# Waydroid (Android apps) is not in Debian: its nft networking drop-in is installed so `apt install waydroid` from
# the Waydroid repository works later
if [ -f /tmp/waydroid-nft.conf ]; then
  mkdir -p /etc/systemd/system/waydroid-container.service.d
  mv /tmp/waydroid-nft.conf /etc/systemd/system/waydroid-container.service.d/duo-nft.conf
fi

systemctl enable nftables ssh sddm NetworkManager > /dev/null 2>&1 || true
# ModemManager: started by duo-modem once the modem is in low-power (never straight online - the QLink crash)
systemctl disable ModemManager > /dev/null 2>&1 || true
# as on pmOS: rmtfs + tqftpserv on; the pd-mapper daemon and qrtr-ns OFF (both are in the kernel)
systemctl disable pd-mapper qrtr-ns > /dev/null 2>&1 || true
# Debian's own hexagonrpcd unit off: duo-sensors starts it with the Duo's arguments
systemctl disable hexagonrpcd > /dev/null 2>&1 || true
echo "setup done: session=$S"
