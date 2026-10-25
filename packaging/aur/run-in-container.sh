#!/bin/bash
# Runs INSIDE an Arch Linux container (see README.md): builds the package from /pkg/PKGBUILD, checks it with namcap,
# installs it and starts it once. Results (the PKGBUILD with its checksum filled in, .SRCINFO, the package) go to /pkg/out.
set -euo pipefail

pacman -Syu --noconfirm --needed base-devel git namcap pacman-contrib sudo
id builder >/dev/null 2>&1 || useradd -m builder
echo 'builder ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/builder

install -o builder -g builder -m644 /pkg/PKGBUILD /home/builder/PKGBUILD
su builder -c '
  set -euo pipefail
  cd ~
  updpkgsums                              # the real checksum of the release archive goes into the PKGBUILD
  makepkg -f --syncdeps --noconfirm       # builds (a few minutes) and packages
  makepkg --printsrcinfo > .SRCINFO       # what the AUR wants next to the PKGBUILD
  echo "=== namcap (warnings are normal, errors are not) ==="
  namcap PKGBUILD || true
  namcap *.pkg.tar.zst || true
'

echo "=== install and smoke test ==="
pacman -U --noconfirm /home/builder/mousiki-plus-*.pkg.tar.zst
mousiki --version
lala --version
mousiki --help | head -5
mousiki --cmd --help | head -3
ls -l /usr/bin/mousiki /usr/bin/lala /usr/lib/mousiki /usr/lib/mousiki/scripts
test -x /usr/lib/mousiki/scripts/fpcalc
pacman -Qi mousiki-plus | grep -E "^(Name|Version|Depends On|Optional Deps)"

mkdir -p /pkg/out
cp /home/builder/PKGBUILD /home/builder/.SRCINFO /home/builder/mousiki-plus-*.pkg.tar.zst /pkg/out/
echo
echo "Done. The PKGBUILD with the checksum and .SRCINFO are in the out folder next to this script."
