#!/usr/bin/env bash
# Assemble openchimed_<version>_<arch>.deb from an already-built binary.
#
# dpkg-deb, not debhelper: this is one binary, one unit, one config file and
# three maintainer scripts. debhelper would add a build-dependency chain and a
# rules file to produce the same archive, and the repo's posture is a
# hand-written Makefile with no autotools (ARCH-35).
#
# It packages a binary rather than building one, deliberately. The release builds
# each architecture once against the oldest glibc it supports and packages that
# same binary as .deb, .rpm and .tar.gz -- so all three carry an identical
# artifact rather than three builds that merely came from the same source.
#
#   build-deb.sh <version> <arch> <binary> [outdir]
set -euo pipefail

VERSION="${1:?usage: build-deb.sh <version> <arch> <binary> [outdir]}"
ARCH="${2:?missing arch (amd64|arm64)}"
BINARY="${3:?missing path to the built openchimed}"
OUTDIR="${4:-dist}"

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MBEDTLS_DIR="${MBEDTLS_DIR:-${root}/third_party/mbedtls-3.6.2}"

[ -x "$BINARY" ] || { echo "build-deb.sh: $BINARY is not an executable" >&2; exit 1; }
case "$ARCH" in amd64|arm64) ;; *) echo "build-deb.sh: unsupported arch $ARCH" >&2; exit 1 ;; esac

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
# mktemp -d gives 0700, and that mode lands on the archive's root entry -- which
# is `/` on the installing system. Every directory in the package must carry the
# mode it should have when unpacked, starting with this one.
chmod 0755 "$stage"

install -D -m 0755 "$BINARY"                                  "$stage/usr/bin/openchimed"
install -D -m 0644 "$root/packaging/debian/openchimed.service" "$stage/lib/systemd/system/openchimed.service"
install -D -m 0644 "$root/packaging/openchimed.env"            "$stage/etc/openchime/openchimed.env"

# Attribution for the linked third-party components. Debian looks for this exact
# path, and a binary package without it is one that should not be published.
install -D -m 0644 /dev/null "$stage/usr/share/doc/openchimed/copyright"
"$root/packaging/licenses.sh" "$MBEDTLS_DIR" > "$stage/usr/share/doc/openchimed/copyright"

for s in postinst prerm postrm; do
  install -D -m 0755 "$root/packaging/debian/$s" "$stage/DEBIAN/$s"
done

# Marks the env file as configuration, which is what makes dpkg preserve an
# operator's edits across an upgrade and prompt rather than overwrite.
printf '/etc/openchime/openchimed.env\n' > "$stage/DEBIAN/conffiles"
chmod 0644 "$stage/DEBIAN/conffiles"

# In kilobytes, as the field expects. Wrong here is cosmetic, but apt shows it
# to the operator before they agree to the install.
size_kb=$(du -ks "$stage" | cut -f1)

cat > "$stage/DEBIAN/control" <<EOF
Package: openchimed
Version: ${VERSION}
Section: net
Priority: optional
Architecture: ${ARCH}
Maintainer: Danny Heskett <dan@danheskett.com>
Installed-Size: ${size_kb}
Depends: libsqlite3-0
Recommends: ca-certificates
Homepage: https://openchime.io
Description: OpenChime chat daemon
 The OpenChime server: a single-binary chat daemon holding one tenant's
 messages in a local SQLite database, speaking a binary protocol over TLS
 with certificate pinning.
 .
 Runs stand-alone with no dependency on any external service, or federated,
 opting in per function to OIDC login, mobile push delivery and the app
 directory. In both cases the daemon holds all message data locally.
 .
 Configuration is entirely by environment variable; the package ships an
 EnvironmentFile at /etc/openchime/openchimed.env.
EOF
chmod 0644 "$stage/DEBIAN/control"

# md5sums lets `dpkg --verify` and debsums detect a tampered or corrupted
# install. DEBIAN/ itself is metadata and is excluded.
( cd "$stage" && find . -type f ! -path './DEBIAN/*' -printf '%P\0' \
  | xargs -0 md5sum > DEBIAN/md5sums )
chmod 0644 "$stage/DEBIAN/md5sums"

mkdir -p "$OUTDIR"
out="${OUTDIR}/openchimed_${VERSION}_${ARCH}.deb"
# --root-owner-group so the archive records root:root regardless of who built
# it; without it a CI runner's uid ends up baked into the package.
dpkg-deb --build --root-owner-group "$stage" "$out" >/dev/null

echo "$out"
