#!/usr/bin/env bash
# Lay out the container image's build context from a release tarball: the
# daemon, its speech data and the web client's wasm as the tarball carries
# them, plus the entrypoint, and Litestream -- the replicator a hosted
# deployment turns on, configured by the entrypoint at boot (ARCH-3,
# ARCH-39). The Dockerfile copies these in and nothing else (no compile, no
# RUN), which is what lets an arm64 image be built on an x86 machine without
# emulation: Litestream is fetched here, for the tarball's architecture,
# rather than installed inside.
#
#   packaging/image-context.sh openchimed-<version>-linux-<arch>.tar.gz <dir>
#   buildah build --arch <arch> -f Dockerfile -t openchime:<version>-<arch> <dir>
set -euo pipefail
TARBALL="${1:?usage: image-context.sh <tarball> <dir>}"
DIR="${2:?usage: image-context.sh <tarball> <dir>}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
rm -rf "$DIR" && mkdir -p "$DIR"
tar -xzf "$TARBALL" -C "$DIR" --strip-components=1
[ -x "$DIR/openchimed" ] || { echo "image-context: no openchimed in $TARBALL" >&2; exit 1; }
cp "$HERE/entrypoint.sh" "$DIR/entrypoint.sh"

# Litestream, a pinned release checked against the digests its release
# publishes (checksums.txt beside the assets) before anything is unpacked: a
# changed upstream file fails here rather than shipping. Its binary is static,
# so it needs nothing of the base. The control plane pins the same release.
# Bump the version and both digests together (VENDORS.md section 4).
LITESTREAM_VERSION=0.5.17
case "$(basename "$TARBALL")" in
  *-linux-amd64.tar.gz) ls_arch=x86_64 ls_sha256=cfb371176d164437ae869f8351cfde49bd1804ae71c61923f75c9cba9c9c006d ;;
  *-linux-arm64.tar.gz) ls_arch=arm64  ls_sha256=f8ca4a050095c1efbda2c4365172e61bf9d955ea0d9ac42f448b52e51819baa5 ;;
  *) echo "image-context: cannot tell the architecture of $TARBALL (want ...-linux-amd64 or ...-linux-arm64.tar.gz)" >&2; exit 1 ;;
esac
ls_name="litestream-${LITESTREAM_VERSION}-linux-${ls_arch}.tar.gz"
ls_tmp="$(mktemp -d)"
trap 'rm -rf "$ls_tmp"' EXIT
curl -fsSL --retry 3 -o "$ls_tmp/$ls_name" \
  "https://github.com/benbjohnson/litestream/releases/download/v${LITESTREAM_VERSION}/${ls_name}"
(cd "$ls_tmp" && echo "${ls_sha256}  ${ls_name}" | sha256sum -c -)
tar -xzf "$ls_tmp/$ls_name" -C "$ls_tmp" litestream LICENSE
install -m 755 "$ls_tmp/litestream" "$DIR/litestream"
cp "$ls_tmp/LICENSE" "$DIR/litestream-LICENSE"
# The base has no CA bundle (the daemon compiles its roots in, ARCH-10), and
# Litestream verifies the bucket's HTTPS against one: it gets the daemon's own
# roots, Mozilla's, written back out as PEM from the generated source, so the
# two trust exactly the same set.
sed -n 's/^"\(.*\)\\n"$/\1/p' "$HERE/third_party/ca-roots/ca_roots.c" > "$DIR/ca-roots.pem"
grep -q 'BEGIN CERTIFICATE' "$DIR/ca-roots.pem" || { echo "image-context: no roots read from third_party/ca-roots/ca_roots.c" >&2; exit 1; }

echo "image-context: $DIR holds openchimed$([ -d "$DIR/tts" ] && echo ', tts/')$([ -d "$DIR/stt" ] && echo ', stt/')$([ -d "$DIR/web" ] && echo ', web/'), the entrypoint and Litestream ${LITESTREAM_VERSION} (${ls_arch})"
