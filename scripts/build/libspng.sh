#!/usr/bin/env bash
# Build the committed libspng (static), with the committed miniz as its zlib,
# from third_party/libspng -- the v0.7.4 release as randy408 published it -- and
# third_party/miniz -- the 3.1.2 release's amalgamated miniz.c and miniz.h --
# both committed (docs/details/VENDORS.md §1), into third_party/libspng-<version>[-arch].
# Nothing is downloaded.
#
#   scripts/build/libspng.sh           native (Linux; the daemon and `make test` link it)
#
# The daemon's PNG decoder and encoder for image previews (ARCH-137,
# docs/details/IMAGE-PREVIEWS.md). libspng is one source file and miniz is one,
# so both are compiled as plain C by the compiler make names; upstream's Meson
# and CMake projects are not used. SPNG_USE_MINIZ points libspng at miniz's
# zlib-compatible API, so no system zlib is linked. No client links it.
set -euo pipefail
unset MAKEFLAGS MFLAGS

LIBSPNG_VERSION=0.7.4

cd "$(dirname "$0")/../.."
mkdir -p third_party
cd third_party

SPNG="$(pwd)/libspng/spng"
MINIZ="$(pwd)/miniz"
[ -f "${SPNG}/spng.c" ] || { echo "build_libspng: third_party/libspng is missing" >&2; exit 1; }
[ -f "${MINIZ}/miniz.c" ] || { echo "build_libspng: third_party/miniz is missing" >&2; exit 1; }

DEST="libspng-${LIBSPNG_VERSION}${OC_ARCH:+-$OC_ARCH}"   # OC_ARCH: another architecture, with make's CC
CC="${CC:-cc}"
AR="${AR:-ar}"
[ -n "${OC_ARCH:-}" ] && AR="${OC_ARCH}-linux-gnu-ar"

if [ ! -f "${DEST}/lib/libspng.a" ]; then
  echo "build_libspng: building ${DEST} (static, with miniz)"
  rm -rf "${DEST}"
  mkdir -p "${DEST}/build" "${DEST}/lib" "${DEST}/include"
  FLAGS=(-std=c99 -O2 -fPIC -w -DSPNG_STATIC -DSPNG_USE_MINIZ -DMINIZ_NO_STDIO -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_TIME)
  "${CC}" "${FLAGS[@]}" -I"${MINIZ}" -c "${MINIZ}/miniz.c" -o "${DEST}/build/miniz.o"
  "${CC}" "${FLAGS[@]}" -I"${MINIZ}" -I"${SPNG}" -c "${SPNG}/spng.c" -o "${DEST}/build/spng.o"
  "${AR}" rcs "${DEST}/lib/libspng.a" "${DEST}/build/spng.o" "${DEST}/build/miniz.o"
  cp "${SPNG}/spng.h" "${DEST}/include/"
fi

echo "build_libspng: done: third_party/${DEST}/lib/libspng.a, header in third_party/${DEST}/include"
