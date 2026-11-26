#!/usr/bin/env bash
# Assemble openchime-<version>-1.<arch>.rpm, the Linux client, from an
# already-built binary: the GUI (make linux-gui), its desktop entry, AppStream
# metadata and icons (packaging/linux-client/), by packaging/rpm/openchime.spec.
#
# The shape of build-rpm.sh, the daemon's, and for its reasons: rpmbuild for
# both architectures on an x86 runner, the package's architecture told rather
# than taken from the machine, and the binary packaged as handed over, so the
# .rpm and the .deb carry the identical file.
#
#   build-client-rpm.sh <version> <arch> <binary> [outdir]      arch: amd64|arm64
set -euo pipefail

VERSION="${1:?usage: build-client-rpm.sh <version> <arch> <binary> [outdir]}"
ARCH="${2:?missing arch (amd64|arm64)}"
BINARY="${3:?missing path to the built client}"
OUTDIR="${4:-dist}"
case "$ARCH" in
  amd64) target=x86_64 ;;
  arm64) target=aarch64 ;;
  *) echo "build-client-rpm.sh: unsupported arch $ARCH" >&2; exit 1 ;;
esac

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
files="$root/packaging/linux-client"
APP_ID=io.openchime.OpenChime

[ -x "$BINARY" ] || { echo "build-client-rpm.sh: $BINARY is not an executable" >&2; exit 1; }
command -v rpmbuild >/dev/null || { echo "build-client-rpm.sh: rpmbuild not found" >&2; exit 1; }

stage="$(mktemp -d)"
topdir="$(mktemp -d)"
trap 'rm -rf "$stage" "$topdir"' EXIT
chmod 0755 "$stage"

# The spec installs from this one directory, so everything it needs is staged
# under names it can predict.
install -m 0755 "$BINARY"                       "$stage/openchime"
install -m 0644 "$files/$APP_ID.desktop"        "$stage/$APP_ID.desktop"
install -m 0644 "$files/$APP_ID.metainfo.xml"   "$stage/$APP_ID.metainfo.xml"
cp -r "$files/icons/hicolor"                    "$stage/hicolor"
install -m 0644 "$root/LICENSE"                 "$stage/LICENSE"

mkdir -p "$topdir"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
rpmbuild -bb --target "${target}" \
  --define "_topdir ${topdir}" \
  --define "_version ${VERSION}" \
  --define "_bindir_src ${stage}" \
  --define "dist %{nil}" \
  "$root/packaging/rpm/openchime.spec" >/dev/null

mkdir -p "$OUTDIR"
built="$(find "$topdir/RPMS" -name '*.rpm' -type f | head -1)"
[ -n "$built" ] || { echo "build-client-rpm.sh: rpmbuild produced no package" >&2; exit 1; }
case "$(basename "$built")" in
  "openchime-${VERSION}-1.${target}.rpm") ;;
  *) echo "build-client-rpm.sh: rpmbuild named the package $(basename "$built"), not openchime-${VERSION}-1.${target}.rpm" >&2; exit 1 ;;
esac
cp "$built" "$OUTDIR/"

echo "${OUTDIR}/$(basename "$built")"
