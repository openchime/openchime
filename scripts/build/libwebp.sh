#!/usr/bin/env bash
# Build the committed libwebp (static) from third_party/libwebp -- the 1.6.0
# release tarball as the WebM project published it, committed
# (docs/VENDORS.md §1) -- into third_party/libwebp-<version>[-arch]. Nothing is
# downloaded.
#
#   scripts/build/libwebp.sh           native (Linux; the daemon and `make test` link it)
#
# The daemon's WebP decoder and encoder for image previews (ARCH-137,
# docs/Plans/IMAGE-PREVIEWS.md): libwebp and libsharpyuv alone. No tools, no
# extras, no mux or demux libraries, no threads (the transfer worker is the
# thread). No client links it.
set -euo pipefail
# One compile at a time (docs/TESTING.md, "One build at a time").
unset MAKEFLAGS MFLAGS

LIBWEBP_VERSION=1.6.0

cd "$(dirname "$0")/../.."
mkdir -p third_party
cd third_party

SRC="$(pwd)/libwebp"
[ -f "${SRC}/CMakeLists.txt" ] || { echo "build_libwebp: third_party/libwebp is missing" >&2; exit 1; }

DEST="libwebp-${LIBWEBP_VERSION}${OC_ARCH:+-$OC_ARCH}"   # OC_ARCH: another architecture, with make's CC
CROSS=()
if [ -n "${OC_ARCH:-}" ]; then
  CROSS=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR="${OC_ARCH}")
fi

if [ ! -f "${DEST}/lib/libwebp.a" ]; then
  echo "build_libwebp: building ${DEST} (static)"
  rm -rf "${DEST}"
  mkdir -p "${DEST}/build"
  # Out of tree: the committed sources are only read.
  cmake -S "${SRC}" -B "${DEST}/build" "${CROSS[@]}" \
    -DCMAKE_C_COMPILER="${CC:-cc}" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$(pwd)/${DEST}" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
    -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
    -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
    -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_LIBWEBPMUX=OFF -DWEBP_BUILD_WEBPMUX=OFF \
    -DWEBP_BUILD_EXTRAS=OFF -DWEBP_USE_THREAD=OFF >/dev/null
  cmake --build "${DEST}/build" -j1 >/dev/null
  cmake --install "${DEST}/build" >/dev/null
fi

echo "build_libwebp: done: third_party/${DEST}/lib/libwebp.a, headers in third_party/${DEST}/include"
