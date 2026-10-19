#!/usr/bin/env bash
# Build the committed libvpx (VP9 only, static) from third_party/libvpx -- the
# release tarball as WebM published it, committed (docs/VENDORS.md §1) -- into
# third_party/libvpx-<version>[-arch|-win]. Nothing is downloaded.
#
#   scripts/build/libvpx.sh            native (Linux; `make test` links it)
#   scripts/build/libvpx.sh windows    cross-built with mingw-w64 for the Win32 client
#
# The codec is the client's alone (ARCH-87, ARCH-110): the daemon never links it.
# scripts/build/media_wasm.sh builds the same sources for the web.
set -euo pipefail

TARGET="${1:-native}"
# The version committed in third_party/libvpx; the build directories carry it.
LIBVPX_VERSION=1.17.0

[ "${OC_ARCH:-}" = aarch64 ] || command -v nasm >/dev/null || command -v yasm >/dev/null || {
  echo "build_libvpx: nasm (or yasm) is required for libvpx's x86 assembly." >&2
  exit 1
}

cd "$(dirname "$0")/../.."
mkdir -p third_party
cd third_party

SRC="$(pwd)/libvpx"
[ -x "${SRC}/configure" ] || { echo "build_libvpx: third_party/libvpx is missing" >&2; exit 1; }

case "${TARGET}" in
  native)  DEST="libvpx-${LIBVPX_VERSION}${OC_ARCH:+-$OC_ARCH}"; VPX_TARGET="${OC_ARCH:+--target=${OC_ARCH/aarch64/arm64}-linux-gcc}" ;;   # OC_ARCH: another architecture, with make's CC
  windows) DEST="libvpx-${LIBVPX_VERSION}-win"; VPX_TARGET="--target=x86_64-win64-gcc" ;;
  *) echo "build_libvpx: target is native or windows, not ${TARGET}" >&2; exit 2 ;;
esac

if [ ! -f "${DEST}/lib/libvpx.a" ]; then
  echo "build_libvpx: building ${DEST} (VP9 only, real-time, static)"
  mkdir -p "${DEST}/build"
  (
    cd "${DEST}/build"
    [ "${TARGET}" = windows ] && export CROSS=x86_64-w64-mingw32-
    # Out of tree: the committed sources are only read. VP9 only: VP8 is not
    # used anywhere (ARCH-87 ships VP9 alone). Real-time only: nothing here
    # encodes offline, and it drops the two-pass paths.
    "${SRC}/configure" ${VPX_TARGET} --prefix="$(cd .. && pwd)" \
      --disable-vp8 --enable-vp9 --enable-realtime-only \
      --disable-examples --disable-tools --disable-docs --disable-unit-tests \
      --disable-shared --enable-static --enable-pic ${OC_ARCH:+--disable-runtime-cpu-detect} $([ "${OC_ARCH:-}" = aarch64 ] || echo --as=nasm) >/dev/null
    make -j"$(nproc)" >/dev/null
    make install >/dev/null
  )
fi

echo "build_libvpx: done: third_party/${DEST}/lib/libvpx.a, headers in third_party/${DEST}/include"
