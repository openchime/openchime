#!/usr/bin/env bash
# Cross-build the pinned mbedTLS (static, Windows) with mingw-w64 into
# third_party/mbedtls-<ver>-win/ — a separate tree from the native build so the
# two don't clobber each other. Gitignored output; reproduced by this script.
set -euo pipefail

MBEDTLS_VERSION="${MBEDTLS_VERSION:-3.6.2}"
CC="${MINGW_CC:-x86_64-w64-mingw32-gcc}"
AR="${MINGW_AR:-x86_64-w64-mingw32-ar}"

cd "$(dirname "$0")/.."
mkdir -p third_party
cd third_party

SRC="mbedtls-${MBEDTLS_VERSION}-win"
TARBALL="${SRC}.tar.bz2"
URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MBEDTLS_VERSION}/mbedtls-${MBEDTLS_VERSION}.tar.bz2"

if [ ! -f "${SRC}/library/libmbedtls.a" ]; then
  if [ ! -d "${SRC}" ]; then
    echo "build_mbedtls_windows: downloading mbedTLS ${MBEDTLS_VERSION}"
    curl -fsSL -o "${TARBALL}" "${URL}"
    mkdir -p "${SRC}"
    tar -xjf "${TARBALL}" -C "${SRC}" --strip-components=1
    rm -f "${TARBALL}"
  fi
  echo "build_mbedtls_windows: cross-building static libraries (mingw)"
  make -C "${SRC}" -j"$(nproc)" CC="${CC}" AR="${AR}" WINDOWS_BUILD=1 lib
fi

echo "build_mbedtls_windows: done — libs in third_party/${SRC}/library"
