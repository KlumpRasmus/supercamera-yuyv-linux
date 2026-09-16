#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sed -n 's/^PACKAGE_VERSION="\([^"]*\)"/\1/p' "$ROOT/dkms.conf")

if [ -z "$VERSION" ]; then
    echo "Could not determine PACKAGE_VERSION from dkms.conf" >&2
    exit 1
fi

OUTDIR=${1:-"$ROOT/dist"}
PKGNAME="supercamera-yuyv-dkms_${VERSION}-1_all.deb"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM HUP

PKGROOT="$TMP/pkg"
SRC="$PKGROOT/usr/src/supercamera-yuyv-$VERSION"

mkdir -p "$PKGROOT/DEBIAN" "$SRC" "$OUTDIR"

for file in supercamera_yuyv.c Makefile dkms.conf README.md LICENSE THIRD_PARTY.md; do
    install -m 0644 "$ROOT/$file" "$SRC/$file"
done

sed "s/@VERSION@/$VERSION/g" "$ROOT/packaging/debian/control.in" > "$PKGROOT/DEBIAN/control"
for script in postinst prerm postrm; do
    sed "s/@VERSION@/$VERSION/g" "$ROOT/packaging/debian/${script}.in" > "$PKGROOT/DEBIAN/$script"
    chmod 0755 "$PKGROOT/DEBIAN/$script"
done

find "$PKGROOT" -type d -exec chmod 0755 {} +

dpkg-deb --root-owner-group --build "$PKGROOT" "$OUTDIR/$PKGNAME"
echo "$OUTDIR/$PKGNAME"
