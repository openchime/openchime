#!/usr/bin/env bash
# Assemble openchimed-<version>-linux-<arch>.tar.gz from an already-built binary.
#
# This is the channel for everyone apt and dnf do not reach: Arch, Alpine (glibc
# variants), NixOS, SUSE, and -- the case that actually motivated it -- an
# air-gapped operator. ARCH-20 tells a stand-alone operator wanting zero
# dependency on the project to "build from source", but the source build runs
# scripts/build_mbedtls.sh, which downloads mbedTLS from GitHub. So the
# advertised offline path was not offline. This tarball is.
#
#   build-tarball.sh <version> <arch> <binary> [outdir]
set -euo pipefail

VERSION="${1:?usage: build-tarball.sh <version> <arch> <binary> [outdir]}"
ARCH="${2:?missing arch (amd64|arm64)}"
BINARY="${3:?missing path to the built openchimed}"
OUTDIR="${4:-dist}"

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MBEDTLS_DIR="${MBEDTLS_DIR:-${root}/third_party/mbedtls-3.6.2}"

[ -x "$BINARY" ] || { echo "build-tarball.sh: $BINARY is not an executable" >&2; exit 1; }

name="openchimed-${VERSION}-linux-${ARCH}"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
chmod 0755 "$stage"
dir="$stage/$name"
mkdir -p "$dir"

install -m 0755 "$BINARY"                                   "$dir/openchimed"
install -m 0644 "$root/packaging/debian/openchimed.service" "$dir/openchimed.service"
install -m 0644 "$root/packaging/openchimed.env"            "$dir/openchimed.env"
"$root/packaging/licenses.sh" "$MBEDTLS_DIR"              > "$dir/COPYRIGHT"
chmod 0644 "$dir/COPYRIGHT"

cat > "$dir/install.sh" <<'INSTALL'
#!/bin/sh
# Install openchimed from this tarball. Run as root.
#
# Does what the .deb and .rpm do, minus the package database: places the binary,
# the unit and the config, then enables the service. An existing config file is
# never overwritten -- that is the one file you are expected to have edited.
set -eu

[ "$(id -u)" = 0 ] || { echo "install.sh: run as root" >&2; exit 1; }
cd "$(dirname "$0")"

install -D -m 0755 openchimed         /usr/bin/openchimed
install -D -m 0644 openchimed.service /lib/systemd/system/openchimed.service
install -D -m 0644 COPYRIGHT          /usr/share/doc/openchimed/copyright

if [ -f /etc/openchime/openchimed.env ]; then
    echo "install.sh: keeping your /etc/openchime/openchimed.env"
    echo "install.sh: the new default is alongside it as openchimed.env.new"
    install -D -m 0644 openchimed.env /etc/openchime/openchimed.env.new
else
    install -D -m 0644 openchimed.env /etc/openchime/openchimed.env
fi

if [ -d /run/systemd/system ]; then
    systemctl daemon-reload
    systemctl enable --now openchimed.service
    echo
    echo "openchimed is running. The first-run owner setup token:"
    echo "  journalctl -u openchimed | grep -i 'setup token'"
else
    echo "install.sh: systemd not detected; start /usr/bin/openchimed yourself."
    echo "install.sh: it reads its configuration from the environment (CONFIG.md)."
fi
INSTALL
chmod 0755 "$dir/install.sh"

cat > "$dir/README" <<EOF
OpenChime daemon ${VERSION} (linux-${ARCH})

  sudo ./install.sh

Requires glibc 2.34 or newer and libsqlite3. Everything else -- including TLS --
is statically linked, so there is nothing further to fetch. That makes this the
offline install: no package repository, no network.

  Configuration   /etc/openchime/openchimed.env   (every variable: docs/CONFIG.md)
  State           /var/lib/openchime/
  Logs            journalctl -u openchimed

To remove: systemctl disable --now openchimed, then delete /usr/bin/openchimed
and /lib/systemd/system/openchimed.service. /var/lib/openchime holds your
messages -- the daemon has no backup component by design, so that file may be
your only copy. Delete it deliberately or not at all.

Verify this archive against the checksum and signature published beside it.
EOF

mkdir -p "$OUTDIR"
out="${OUTDIR}/${name}.tar.gz"
# Deterministic: fixed owner, sorted order, and a fixed mtime taken from the
# release number rather than "now", so rebuilding the same release produces the
# same bytes.
tar --sort=name --owner=root:0 --group=root:0 --mtime="@0" \
    -czf "$out" -C "$stage" "$name"

( cd "$OUTDIR" && sha256sum "$(basename "$out")" > "$(basename "$out").sha256" )

echo "$out"
