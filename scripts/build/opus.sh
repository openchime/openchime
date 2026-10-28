#!/usr/bin/env bash
# Build the committed libopus (static) from third_party/opus -- the release
# tarball as Xiph published it, committed (docs/VENDORS.md §1) -- into
# third_party/opus-<version>[-arch|-win]. Nothing is downloaded.
#
#   scripts/build/opus.sh            native (Linux; `make test` links it)
#   scripts/build/opus.sh windows    cross-built with mingw-w64 for the Win32 client
#
# The client's codec: the daemon relays call audio without decoding it
# (ARCH-73), and links libopus only for read-aloud (TTS=1, mk/daemon.mk).
# scripts/build/media_wasm.sh builds the same sources for the web.
set -euo pipefail

TARGET="${1:-native}"
# The version committed in third_party/opus; the build directories carry it.
OPUS_VERSION=1.6.1

cd "$(dirname "$0")/../.."
mkdir -p third_party
cd third_party

SRC="$(pwd)/opus"
[ -x "${SRC}/configure" ] || { echo "build_opus: third_party/opus is missing" >&2; exit 1; }

case "${TARGET}" in
  native)  DEST="opus-${OPUS_VERSION}${OC_ARCH:+-$OC_ARCH}"; HOST="${OC_ARCH:+--host=$OC_ARCH-linux-gnu}" ;;   # OC_ARCH: another architecture, with make's CC
  windows) DEST="opus-${OPUS_VERSION}-win"; HOST="--host=x86_64-w64-mingw32" ;;
  *) echo "build_opus: target is native or windows, not ${TARGET}" >&2; exit 2 ;;
esac

if [ ! -f "${DEST}/lib/libopus.a" ]; then
  echo "build_opus: building ${DEST} (static)"
  mkdir -p "${DEST}/build"
  (
    cd "${DEST}/build"
    # Out of tree: the committed sources are only read. The release tarball
    # ships a generated configure, so no autotools are needed.
    "${SRC}/configure" ${HOST} --prefix="$(cd .. && pwd)" \
      --disable-shared --enable-static --with-pic \
      --disable-doc --disable-extra-programs >/dev/null
    make -j"$(nproc)" >/dev/null
    make install >/dev/null
  )
fi

echo "build_opus: done: third_party/${DEST}/lib/libopus.a, headers in third_party/${DEST}/include"
