#!/usr/bin/env bash
# Lay out the container image's build context from a release tarball: the
# daemon, its speech data and the web client's wasm as the tarball carries
# them, plus the entrypoint. The Dockerfile copies these in and nothing else
# (no compile, no RUN), which is what lets an arm64 image be built on an x86
# machine without emulation.
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
echo "image-context: $DIR holds openchimed$([ -d "$DIR/tts" ] && echo ', tts/')$([ -d "$DIR/stt" ] && echo ', stt/')$([ -d "$DIR/web" ] && echo ', web/') and the entrypoint"
