#!/usr/bin/env bash
# Fetch and build a pinned mbedTLS (static libs) into third_party/.
#
# We vendor a fixed version rather than use distro packages because those
# diverge across our build environments (Ubuntu ships mbedTLS 2.28, Alpine
# ships 3.6.x) and their APIs are not source-compatible. Building one pinned
# version from source gives local, CI, and the Docker image the same library.
# The build output under third_party/ is gitignored; this script reproduces it.
set -euo pipefail

MBEDTLS_VERSION="${MBEDTLS_VERSION:-3.6.2}"
cd "$(dirname "$0")/.."
mkdir -p third_party
cd third_party

SRC="mbedtls-${MBEDTLS_VERSION}"
TARBALL="${SRC}.tar.bz2"
URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MBEDTLS_VERSION}/${TARBALL}"

if [ ! -f "${SRC}/library/libmbedtls.a" ]; then
  if [ ! -d "${SRC}" ]; then
    echo "build_mbedtls: downloading ${URL}"
    curl -fsSL -o "${TARBALL}" "${URL}"
    tar -xjf "${TARBALL}"
    rm -f "${TARBALL}"
  fi
  echo "build_mbedtls: building ${SRC} static libraries"
  # `make lib` builds only the libraries (skips programs/tests). The release
  # tarball ships pre-generated sources, so no Python is required.
  make -C "${SRC}" -j"$(nproc)" lib
fi

echo "build_mbedtls: done — libs in third_party/${SRC}/library, headers in third_party/${SRC}/include"
