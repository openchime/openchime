#!/usr/bin/env bash
# Fetch and build a pinned libvpx (VP9 only, static) into third_party/ — the
# "fetched at build" vendoring class (docs/VENDORS.md §2), same shape as
# build_mbedtls.sh: SHA-256-pinned tarball, gitignored output, reproduced here.
#
#   scripts/build_libvpx.sh            native (Linux; `make test` links it)
#   scripts/build_libvpx.sh windows    cross-built with mingw-w64 for the Win32 client
#
# The codec is the client's alone (ARCH-87, ARCH-110): the daemon never links it.
set -euo pipefail

TARGET="${1:-native}"
LIBVPX_VERSION="${LIBVPX_VERSION:-1.17.0}"

# Known-good SHA-256 of the release tarball. WebM publishes a detached GPG
# signature (.asc) rather than a sum: the tarball was verified against the "WebM
# release signing key" (fingerprint 8DA7 0C3B 2338 B012 BAB3 0EE5 1C1C 1A9A 2A45
# D4D4) and this is the sum of that verified file. Override both together when
# bumping LIBVPX_VERSION -- a version without a matching sum is refused rather
# than fetched unverified.
LIBVPX_SHA256_1_17_0="7f98aae5c52c46e85b7b26feabcab9b1554605e771bd1df1d6e6a00ee22fa8ed"
_sum_var="LIBVPX_SHA256_${LIBVPX_VERSION//./_}"
LIBVPX_SHA256="${LIBVPX_SHA256:-${!_sum_var:-}}"
if [ -z "${LIBVPX_SHA256}" ]; then
  echo "build_libvpx: no known SHA-256 for libvpx ${LIBVPX_VERSION}." >&2
  echo "  Verify the release's .asc signature, then pass its sum explicitly:" >&2
  echo "  LIBVPX_SHA256=<sum> $0 ${TARGET}" >&2
  exit 1
fi

verify_tarball() {
  local f="$1" got
  got="$(sha256sum "$f" | cut -d" " -f1)"
  if [ "$got" != "${LIBVPX_SHA256}" ]; then
    echo "build_libvpx: SHA-256 MISMATCH for $f" >&2
    echo "  expected ${LIBVPX_SHA256}" >&2
    echo "  got      ${got}" >&2
    rm -f "$f"
    exit 1
  fi
  echo "build_libvpx: sha256 ok (${got})"
}

command -v nasm >/dev/null || command -v yasm >/dev/null || {
  echo "build_libvpx: nasm (or yasm) is required for libvpx's x86 assembly." >&2
  exit 1
}

cd "$(dirname "$0")/.."
mkdir -p third_party
cd third_party

case "${TARGET}" in
  native)  DEST="libvpx-${LIBVPX_VERSION}";     VPX_TARGET="" ;;
  windows) DEST="libvpx-${LIBVPX_VERSION}-win"; VPX_TARGET="--target=x86_64-win64-gcc" ;;
  *) echo "build_libvpx: target is native or windows, not ${TARGET}" >&2; exit 2 ;;
esac

TARBALL="libvpx-${LIBVPX_VERSION}.tar.gz"
URL="https://storage.googleapis.com/downloads.webmproject.org/releases/webm/${TARBALL}"

if [ ! -f "${DEST}/lib/libvpx.a" ]; then
  if [ ! -d "${DEST}/src" ]; then
    echo "build_libvpx: downloading ${URL}"
    curl -fsSL -o "${TARBALL}" "${URL}"
    verify_tarball "${TARBALL}"
    mkdir -p "${DEST}/src"
    tar -xzf "${TARBALL}" -C "${DEST}/src" --strip-components=1
    rm -f "${TARBALL}"
  fi
  echo "build_libvpx: building ${DEST} (VP9 only, real-time, static)"
  mkdir -p "${DEST}/build"
  (
    cd "${DEST}/build"
    [ "${TARGET}" = windows ] && export CROSS=x86_64-w64-mingw32-
    # VP9 only: VP8 is not used anywhere (ARCH-87 ships VP9 alone). Real-time
    # only: nothing here encodes offline, and it drops the two-pass paths.
    ../src/configure ${VPX_TARGET} --prefix="$(cd .. && pwd)" \
      --disable-vp8 --enable-vp9 --enable-realtime-only \
      --disable-examples --disable-tools --disable-docs --disable-unit-tests \
      --disable-shared --enable-static --enable-pic --as=nasm >/dev/null
    make -j"$(nproc)" >/dev/null
    make install >/dev/null
  )
fi

echo "build_libvpx: done — third_party/${DEST}/lib/libvpx.a, headers in third_party/${DEST}/include"
