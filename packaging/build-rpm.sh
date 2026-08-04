#!/usr/bin/env bash
# Assemble openchimed-<version>-1.<arch>.rpm from an already-built binary.
#
# Must run somewhere with rpmbuild -- in the release that is the same
# rockylinux:9 container that produced the binary, which also gives the package
# the right %{_target_cpu} and the RHEL-family dependency names.
#
#   build-rpm.sh <version> <binary> [outdir]
set -euo pipefail

VERSION="${1:?usage: build-rpm.sh <version> <binary> [outdir]}"
BINARY="${2:?missing path to the built openchimed}"
OUTDIR="${3:-dist}"

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MBEDTLS_DIR="${MBEDTLS_DIR:-${root}/third_party/mbedtls-3.6.2}"

[ -x "$BINARY" ] || { echo "build-rpm.sh: $BINARY is not an executable" >&2; exit 1; }
command -v rpmbuild >/dev/null || { echo "build-rpm.sh: rpmbuild not found" >&2; exit 1; }

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
chmod 0755 "$stage"

# The spec installs from this one directory, so everything it needs is staged
# under flat names it can predict.
install -m 0755 "$BINARY"                                      "$stage/openchimed"
install -m 0644 "$root/packaging/debian/openchimed.service"    "$stage/openchimed.service"
install -m 0644 "$root/packaging/openchimed.env"               "$stage/openchimed.env"
"$root/packaging/licenses.sh" "$MBEDTLS_DIR"                 > "$stage/copyright"
chmod 0644 "$stage/copyright"

topdir="$(mktemp -d)"
trap 'rm -rf "$stage" "$topdir"' EXIT
mkdir -p "$topdir"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

rpmbuild -bb \
  --define "_topdir ${topdir}" \
  --define "_version ${VERSION}" \
  --define "_bindir_src ${stage}" \
  --define "dist %{nil}" \
  "$root/packaging/rpm/openchimed.spec" >/dev/null

mkdir -p "$OUTDIR"
built="$(find "$topdir/RPMS" -name '*.rpm' -type f | head -1)"
[ -n "$built" ] || { echo "build-rpm.sh: rpmbuild produced no package" >&2; exit 1; }
cp "$built" "$OUTDIR/"

echo "${OUTDIR}/$(basename "$built")"
