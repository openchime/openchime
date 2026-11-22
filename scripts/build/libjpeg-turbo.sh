#!/usr/bin/env bash
# Build the committed libjpeg-turbo (static) from third_party/libjpeg-turbo --
# the 3.2.1 release tarball as the libjpeg-turbo Project published it,
# committed (docs/VENDORS.md §1) -- into third_party/libjpeg-turbo-<version>[-arch].
# Nothing is downloaded.
#
#   scripts/build/libjpeg-turbo.sh     native (Linux; the daemon and `make test` link it)
#
# The daemon's JPEG decoder and encoder for image previews (ARCH-137,
# docs/Plans/IMAGE-PREVIEWS.md): the libjpeg API alone, 8-bit, with its SIMD
# (nasm on x86_64, NEON on aarch64). No TurboJPEG API, no tools, no tests, no
# shared library. No client links it.
set -euo pipefail
# One compile at a time (docs/TESTING.md, "One build at a time").
unset MAKEFLAGS MFLAGS

LIBJPEG_TURBO_VERSION=3.2.1

cd "$(dirname "$0")/../.."
mkdir -p third_party
cd third_party

SRC="$(pwd)/libjpeg-turbo"
[ -f "${SRC}/CMakeLists.txt" ] || { echo "build_libjpeg_turbo: third_party/libjpeg-turbo is missing" >&2; exit 1; }

DEST="libjpeg-turbo-${LIBJPEG_TURBO_VERSION}${OC_ARCH:+-$OC_ARCH}"   # OC_ARCH: another architecture, with make's CC
CROSS=()
if [ -n "${OC_ARCH:-}" ]; then
  CROSS=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR="${OC_ARCH}")
fi

if [ ! -f "${DEST}/lib/libjpeg.a" ]; then
  echo "build_libjpeg_turbo: building ${DEST} (static, libjpeg API)"
  rm -rf "${DEST}"
  mkdir -p "${DEST}/build"
  # Out of tree: the committed sources are only read.
  cmake -S "${SRC}" -B "${DEST}/build" "${CROSS[@]}" \
    -DCMAKE_C_COMPILER="${CC:-cc}" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$(pwd)/${DEST}" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DENABLE_SHARED=0 -DENABLE_STATIC=1 -DWITH_TURBOJPEG=0 \
    -DWITH_TOOLS=0 -DWITH_TESTS=0 -DWITH_FUZZ=0 >/dev/null
  cmake --build "${DEST}/build" -j1 >/dev/null
  cmake --install "${DEST}/build" >/dev/null
fi

echo "build_libjpeg_turbo: done: third_party/${DEST}/lib/libjpeg.a, headers in third_party/${DEST}/include"
