#!/usr/bin/env bash
# Assemble openchime_<version>_<arch>.deb, the Linux client, from an
# already-built binary: the GUI (make linux-gui), its desktop entry, AppStream
# metadata and icons (packaging/linux-client/).
#
# The shape of build-deb.sh, the daemon's, and for its reasons: dpkg-deb, not
# debhelper, and a binary packaged rather than built, so the .deb and the .rpm
# carry the identical file. The client is a package of its own, beside
# openchimed, so a desktop installs it without the server.
#
# Its dependencies are the system libraries it links, under Debian's and
# Ubuntu's names (docs/details/VENDORS.md §3). glibc's version is read from the
# binary: the newest symbol version it needs. The client builds on Ubuntu
# 24.04 (glibc 2.39), so it installs on Ubuntu 24.04 and Debian 13 and later,
# whose names these are (ARCH-20). No maintainer scripts: the menu and the icon
# cache are refreshed by the triggers of the packages that own them.
#
#   build-client-deb.sh <version> <arch> <binary> [outdir]      arch: amd64|arm64
set -euo pipefail

VERSION="${1:?usage: build-client-deb.sh <version> <arch> <binary> [outdir]}"
ARCH="${2:?missing arch (amd64|arm64)}"
BINARY="${3:?missing path to the built client}"
OUTDIR="${4:-dist}"

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
files="$root/packaging/linux-client"
APP_ID=io.openchime.OpenChime

[ -x "$BINARY" ] || { echo "build-client-deb.sh: $BINARY is not an executable" >&2; exit 1; }
case "$ARCH" in amd64|arm64) ;; *) echo "build-client-deb.sh: unsupported arch $ARCH" >&2; exit 1 ;; esac

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
# mktemp -d gives 0700, and that mode lands on the archive's root entry (see
# build-deb.sh): every directory carries the mode it should have unpacked.
chmod 0755 "$stage"

install -D -m 0755 "$BINARY"                               "$stage/usr/bin/openchime"
install -D -m 0644 "$files/$APP_ID.desktop"                "$stage/usr/share/applications/$APP_ID.desktop"
install -D -m 0644 "$files/$APP_ID.metainfo.xml"           "$stage/usr/share/metainfo/$APP_ID.metainfo.xml"
mkdir -p "$stage/usr/share/icons"
cp -r "$files/icons/hicolor" "$stage/usr/share/icons/"
# The project's licence, at the path Debian looks for.
install -D -m 0644 "$root/LICENSE"                         "$stage/usr/share/doc/openchime/copyright"
find "$stage/usr" -type d -exec chmod 0755 {} +
find "$stage/usr/share" -type f -exec chmod 0644 {} +

# The newest glibc symbol version the binary needs is the least glibc it runs
# on; readelf reads either architecture's ELF.
glibc=$(readelf --dyn-syms --wide "$BINARY" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -1)
[ -n "$glibc" ] || { echo "build-client-deb.sh: $BINARY needs no versioned glibc symbol: not the dynamically linked client" >&2; exit 1; }

mkdir -p "$stage/DEBIAN"
size_kb=$(du -ks "$stage" | cut -f1)
cat > "$stage/DEBIAN/control" <<EOF
Package: openchime
Version: ${VERSION}
Section: net
Priority: optional
Architecture: ${ARCH}
Maintainer: Danny Heskett <dan@danheskett.com>
Installed-Size: ${size_kb}
Depends: libc6 (>= ${glibc#GLIBC_}), libfreetype6, libharfbuzz0b, libfontconfig1, libpipewire-0.3-0t64, libglib2.0-0t64, libsecret-1-0
Homepage: https://openchime.io
Description: OpenChime chat client
 The desktop client for an OpenChime server: messaging, channels, threads
 and search; calls with screen sharing and cameras, encrypted end to end;
 recorded video messages; and read-aloud, voice input and conversation
 summaries, all computed on the workspace's own server.
 .
 It runs on Wayland and X11, keeps its session in the desktop's keyring, and
 opens openchime:// links.
EOF
chmod 0644 "$stage/DEBIAN/control"

( cd "$stage" && find . -type f ! -path './DEBIAN/*' -printf '%P\0' \
  | xargs -0 md5sum > DEBIAN/md5sums )
chmod 0644 "$stage/DEBIAN/md5sums"

mkdir -p "$OUTDIR"
out="${OUTDIR}/openchime_${VERSION}_${ARCH}.deb"
# --root-owner-group, as build-deb.sh: root:root whoever built it.
dpkg-deb --build --root-owner-group "$stage" "$out" >/dev/null

echo "$out"
