#!/usr/bin/env bash
# Assemble openchimed-<version>-1.<arch>.rpm from an already-built binary.
#
# Runs on the release runner with rpmbuild, for BOTH architectures: the
# arm64 binary is cross-built on the x86 runner (release.yml), so the
# package's architecture is told, not taken from the machine. Left to
# rpmbuild's default it was the host's, and release 21's arm64 package came
# out as openchimed-21-1.x86_64.rpm, colliding with the real one in the pool.
#
#   build-rpm.sh <version> <arch> <binary> [outdir]      arch: amd64|arm64
set -euo pipefail

VERSION="${1:?usage: build-rpm.sh <version> <arch> <binary> [outdir]}"
ARCH="${2:?missing arch (amd64|arm64)}"
BINARY="${3:?missing path to the built openchimed}"
OUTDIR="${4:-dist}"
case "$ARCH" in
  amd64) target=x86_64 ;;
  arm64) target=aarch64 ;;
  *) echo "build-rpm.sh: unsupported arch $ARCH" >&2; exit 1 ;;
esac

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
# Read-aloud's data, when it was built beside the binary (see build-deb.sh).
with_tts=0
if [ -f "$(dirname "$BINARY")/tts/manifest" ]; then
  cp -r "$(dirname "$BINARY")/tts" "$stage/tts"
  with_tts=1
fi
# And voice input's recognizer data (ARCH-112).
with_stt=0
if [ -f "$(dirname "$BINARY")/stt/manifest" ]; then
  cp -r "$(dirname "$BINARY")/stt" "$stage/stt"
  with_stt=1
fi
# And the web client the daemon serves (docs/WEB.md).
with_web=0
if [ -f "$(dirname "$BINARY")/web/openchime.wasm" ]; then
  cp -r "$(dirname "$BINARY")/web" "$stage/web"
  with_web=1
fi
install -m 0644 "$root/packaging/debian/openchimed.service"    "$stage/openchimed.service"
install -m 0600 "$root/packaging/openchimed.env"               "$stage/openchimed.env"
# The licence file (packaging/licenses.sh) is assembled from the vendored
# sources, which exist only where the build ran. A candidate's tree carries it
# assembled, beside the binary, so the release can package without them.
copyright_into() {
  if [ -f "$(dirname "$BINARY")/copyright" ]; then cp "$(dirname "$BINARY")/copyright" "$1"
  else "$root/packaging/licenses.sh" "$MBEDTLS_DIR" > "$1"; fi
}
copyright_into "$stage/copyright"
chmod 0644 "$stage/copyright"

topdir="$(mktemp -d)"
trap 'rm -rf "$stage" "$topdir"' EXIT
mkdir -p "$topdir"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

rpmbuild -bb --target "${target}" \
  --define "_topdir ${topdir}" \
  --define "_version ${VERSION}" \
  --define "_bindir_src ${stage}" \
  --define "_with_tts ${with_tts}" \
  --define "_with_stt ${with_stt}" \
  --define "_with_web ${with_web}" \
  --define "dist %{nil}" \
  "$root/packaging/rpm/openchimed.spec" >/dev/null

mkdir -p "$OUTDIR"
built="$(find "$topdir/RPMS" -name '*.rpm' -type f | head -1)"
[ -n "$built" ] || { echo "build-rpm.sh: rpmbuild produced no package" >&2; exit 1; }
case "$(basename "$built")" in
  *".${target}.rpm") ;;
  *) echo "build-rpm.sh: rpmbuild named the package $(basename "$built"), not .${target}.rpm" >&2; exit 1 ;;
esac
cp "$built" "$OUTDIR/"

echo "${OUTDIR}/$(basename "$built")"
