#!/usr/bin/env bash
# Fetch and build a pinned speexdsp (static) into third_party/ — the "fetched at
# build" vendoring class (docs/VENDORS.md §2), the shape of build_opus.sh.
#
#   scripts/build_speexdsp.sh            native (Linux; `make test` links it)
#   scripts/build_speexdsp.sh windows    cross-built with mingw-w64 for the Win32 client
#
# Client-side only: its acoustic echo canceller keeps the client's own playback
# out of the microphone for voice input (ARCH-112, docs/AUDIO.md §6). The daemon
# never links it.
set -euo pipefail

TARGET="${1:-native}"
SPEEXDSP_VERSION="${SPEEXDSP_VERSION:-1.2.1}"

# Known-good SHA-256 of the release tarball Xiph publishes. Override both
# together when bumping -- a version without a matching sum is refused rather
# than fetched unverified.
SPEEXDSP_SHA256_1_2_1="8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d"
_sum_var="SPEEXDSP_SHA256_${SPEEXDSP_VERSION//./_}"
SPEEXDSP_SHA256="${SPEEXDSP_SHA256:-${!_sum_var:-}}"
if [ -z "${SPEEXDSP_SHA256}" ]; then
  echo "build_speexdsp: no known SHA-256 for speexdsp ${SPEEXDSP_VERSION}." >&2
  echo "  SPEEXDSP_SHA256=<sum> $0 ${TARGET}" >&2
  exit 1
fi

cd "$(dirname "$0")/.."
mkdir -p third_party
cd third_party

case "${TARGET}" in
  native)  DEST="speexdsp-${SPEEXDSP_VERSION}";     HOST="" ;;
  windows) DEST="speexdsp-${SPEEXDSP_VERSION}-win"; HOST="--host=x86_64-w64-mingw32" ;;
  *) echo "build_speexdsp: target is native or windows, not ${TARGET}" >&2; exit 2 ;;
esac

TARBALL="speexdsp-${SPEEXDSP_VERSION}.tar.gz"
URL="https://downloads.xiph.org/releases/speex/${TARBALL}"

if [ ! -f "${DEST}/lib/libspeexdsp.a" ]; then
  if [ ! -d "${DEST}/src" ]; then
    echo "build_speexdsp: downloading ${URL}"
    curl -fsSL -o "${TARBALL}" "${URL}"
    got="$(sha256sum "${TARBALL}" | cut -d" " -f1)"
    if [ "${got}" != "${SPEEXDSP_SHA256}" ]; then
      echo "build_speexdsp: SHA-256 MISMATCH for ${TARBALL}" >&2
      echo "  expected ${SPEEXDSP_SHA256}" >&2
      echo "  got      ${got}" >&2
      rm -f "${TARBALL}"
      exit 1
    fi
    mkdir -p "${DEST}/src"
    tar -xzf "${TARBALL}" -C "${DEST}/src" --strip-components=1
    rm -f "${TARBALL}"
  fi
  echo "build_speexdsp: building ${DEST} (static, floating point)"
  mkdir -p "${DEST}/build"
  (
    cd "${DEST}/build"
    # The release tarball ships a generated configure, so no autotools needed.
    ../src/configure ${HOST} --prefix="$(cd .. && pwd)" \
      --disable-shared --enable-static --with-pic --disable-examples >/dev/null
    make -j"$(nproc)" >/dev/null
    make install >/dev/null
  )
fi

echo "build_speexdsp: done — third_party/${DEST}/lib/libspeexdsp.a, headers in third_party/${DEST}/include"
