#!/bin/sh
# Refresh the aport's source tarball and its checksum from this tree.
#
# The app has no published tarball yet, so pmaports/temp/tap builds from one
# made here. Once the source lands in a repo of its own, point the APKBUILD's
# source= at the usual github tarball and delete this script.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
aport=$here/../../pmaports/temp/tap
version=$(sed -n "s/^ *version: *'\([0-9.]*\)',.*/\1/p" "$here/meson.build")

[ -n "$version" ] || { echo "no version in meson.build" >&2; exit 1; }
[ -d "$aport" ] || { echo "no aport at $aport" >&2; exit 1; }

tarball=tap-$version.tar.gz

tar -czf "$aport/$tarball" \
    --exclude=_build --exclude=.git --exclude="*.tar.gz" \
    --transform "s,^\.,tap-$version," \
    -C "$here" .

sed -i "s|^sha512sums=.*|sha512sums=\"$(cd "$aport" && sha512sum "$tarball")\"|" \
    "$aport/APKBUILD"

echo "$aport/$tarball"
