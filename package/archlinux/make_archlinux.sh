#!/bin/bash
set -euo pipefail

pacman -Syu --noconfirm
pacman -S --needed --noconfirm base-devel git sudo systemd

if ! id builder >/dev/null 2>&1; then
    useradd -m builder
fi

printf 'builder ALL=(ALL:ALL) NOPASSWD: ALL\n' > /etc/sudoers.d/builder
chmod 0440 /etc/sudoers.d/builder

mkdir -p /home/builder/build /home/builder/package /home/builder/project

tar -C /workdir --exclude='./build' --exclude='./package/archlinux' -cf - . | tar -C /home/builder/project -xf -

cp /workdir/package/archlinux/PKGBUILD /home/builder/package/PKGBUILD

chown -R builder:builder /home/builder

cd /home/builder/package

runuser -u builder -- env BUILDDIR=/home/builder/package PKGDEST=/home/builder/build makepkg -s --noconfirm

mkdir -p /workdir/build || true

find /home/builder/build -maxdepth 1 -type f -name '*.pkg.tar.zst' -exec cp -f -t /workdir/build/ {} +
