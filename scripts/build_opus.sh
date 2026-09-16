#!/usr/bin/env bash
# Fetch and build a pinned libopus (static) into third_party/ — the "fetched at
# build" vendoring class (docs/VENDORS.md §2), same shape as build_mbedtls.sh.
#
#   scripts/build_opus.sh            native (Linux; `make test` links it)
#   scripts/build_opus.sh windows    cross-built with mingw-w64 for the Win32 client
#
# Client-side only: the daemon relays Opus without decoding it (ARCH-73) and
# links no codec.
set -euo pipefail

TARGET="${1:-native}"
OPUS_VERSION="${OPUS_VERSION:-1.6.1}"

# Known-good SHA-256, from the SHA256SUMS.txt Xiph publishes beside the release.
# Override both together when bumping OPUS_VERSION -- a version without a
# matching sum is refused rather than fetched unverified.
OPUS_SHA256_1_6_1="6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1"
_sum_var="OPUS_SHA256_${OPUS_VERSION//./_}"
OPUS_SHA256="${OPUS_SHA256:-${!_sum_var:-}}"
if [ -z "${OPUS_SHA256}" ]; then
  echo "build_opus: no known SHA-256 for libopus ${OPUS_VERSION}." >&2
  echo "  curl -fsSL https://downloads.xiph.org/releases/opus/SHA256SUMS.txt" >&2
  echo "  OPUS_SHA256=<sum> $0 ${TARGET}" >&2
  exit 1
fi

verify_tarball() {
  local f="$1" got
  got="$(sha256sum "$f" | cut -d" " -f1)"
  if [ "$got" != "${OPUS_SHA256}" ]; then
    echo "build_opus: SHA-256 MISMATCH for $f" >&2
    echo "  expected ${OPUS_SHA256}" >&2
    echo "  got      ${got}" >&2
    rm -f "$f"
    exit 1
  fi
  echo "build_opus: sha256 ok (${got})"
}

cd "$(dirname "$0")/.."
mkdir -p third_party
cd third_party

case "${TARGET}" in
  native)  DEST="opus-${OPUS_VERSION}";     HOST="" ;;
  windows) DEST="opus-${OPUS_VERSION}-win"; HOST="--host=x86_64-w64-mingw32" ;;
  *) echo "build_opus: target is native or windows, not ${TARGET}" >&2; exit 2 ;;
esac

TARBALL="opus-${OPUS_VERSION}.tar.gz"
URL="https://downloads.xiph.org/releases/opus/${TARBALL}"

if [ ! -f "${DEST}/lib/libopus.a" ]; then
  if [ ! -d "${DEST}/src" ]; then
    echo "build_opus: downloading ${URL}"
    curl -fsSL -o "${TARBALL}" "${URL}"
    verify_tarball "${TARBALL}"
    mkdir -p "${DEST}/src"
    tar -xzf "${TARBALL}" -C "${DEST}/src" --strip-components=1
    rm -f "${TARBALL}"
  fi
  echo "build_opus: building ${DEST} (static)"
  mkdir -p "${DEST}/build"
  (
    cd "${DEST}/build"
    # The release tarball ships a generated configure, so no autotools needed.
    ../src/configure ${HOST} --prefix="$(cd .. && pwd)" \
      --disable-shared --enable-static --with-pic \
      --disable-doc --disable-extra-programs >/dev/null
    make -j"$(nproc)" >/dev/null
    make install >/dev/null
  )
fi

echo "build_opus: done — third_party/${DEST}/lib/libopus.a, headers in third_party/${DEST}/include"
