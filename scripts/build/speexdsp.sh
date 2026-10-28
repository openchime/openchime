#!/usr/bin/env bash
# Build the committed speexdsp (static) from third_party/speexdsp -- the
# release tarball as Xiph published it, committed (docs/VENDORS.md §1) -- into
# third_party/speexdsp-<version>[-arch|-win]. Nothing is downloaded.
#
#   scripts/build/speexdsp.sh            native (Linux; `make test` links it)
#   scripts/build/speexdsp.sh windows    cross-built with mingw-w64 for the Win32 client
#
# Client-side only: its acoustic echo canceller keeps the client's own playback
# out of the microphone for voice input (ARCH-112, docs/AUDIO.md §6). The daemon
# never links it. scripts/build/media_wasm.sh builds the same sources for the web.
set -euo pipefail

TARGET="${1:-native}"
# The version committed in third_party/speexdsp; the build directories carry it.
SPEEXDSP_VERSION=1.2.1

cd "$(dirname "$0")/../.."
mkdir -p third_party
cd third_party

SRC="$(pwd)/speexdsp"
[ -x "${SRC}/configure" ] || { echo "build_speexdsp: third_party/speexdsp is missing" >&2; exit 1; }

case "${TARGET}" in
  native)  DEST="speexdsp-${SPEEXDSP_VERSION}${OC_ARCH:+-$OC_ARCH}"; HOST="${OC_ARCH:+--host=$OC_ARCH-linux-gnu}" ;;   # OC_ARCH: another architecture, with make's CC
  windows) DEST="speexdsp-${SPEEXDSP_VERSION}-win"; HOST="--host=x86_64-w64-mingw32" ;;
  *) echo "build_speexdsp: target is native or windows, not ${TARGET}" >&2; exit 2 ;;
esac

if [ ! -f "${DEST}/lib/libspeexdsp.a" ]; then
  echo "build_speexdsp: building ${DEST} (static, floating point)"
  mkdir -p "${DEST}/build"
  (
    cd "${DEST}/build"
    # Out of tree: the committed sources are only read. The release tarball
    # ships a generated configure, so no autotools are needed.
    "${SRC}/configure" ${HOST} --prefix="$(cd .. && pwd)" \
      --disable-shared --enable-static --with-pic --disable-examples >/dev/null
    make -j"$(nproc)" >/dev/null
    make install >/dev/null
  )
fi

echo "build_speexdsp: done: third_party/${DEST}/lib/libspeexdsp.a, headers in third_party/${DEST}/include"
