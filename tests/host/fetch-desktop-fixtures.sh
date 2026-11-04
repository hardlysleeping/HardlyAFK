#!/bin/sh
# Fetch real .desktop files from Debian 13 arm64 packages for the launcher tests.
# usage: fetch-desktop-fixtures.sh /out/dir
set -eu

out=${1:?usage: fetch-desktop-fixtures.sh /out/dir}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$out"

curl -fsSL --retry 3 -o "$out/Packages.xz" \
  http://deb.debian.org/debian/dists/trixie/main/binary-arm64/Packages.xz
xz -dc "$out/Packages.xz" > "$out/Packages"

python3 "$here/extract_deb_desktop.py" "$out/Packages" "$out/entries" \
  qterminal konsole pcmanfm-qt lximage-qt

ls -1 "$out/entries"
