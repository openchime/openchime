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

# Known-good SHA-256 of the release tarball, taken from the mbedtls-${MBEDTLS_VERSION}
# -sha256sum.txt asset upstream publishes beside it. Override both together
# when bumping MBEDTLS_VERSION -- a version without a matching sum is refused
# rather than fetched unverified, because an unpinned fetch of a TLS library
# is the one dependency where "probably fine" is not an answer.
MBEDTLS_SHA256_3_6_2="8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca"
_sum_var="MBEDTLS_SHA256_${MBEDTLS_VERSION//./_}"
MBEDTLS_SHA256="${MBEDTLS_SHA256:-${!_sum_var:-}}"
if [ -z "${MBEDTLS_SHA256}" ]; then
  echo "build_mbedtls: no known SHA-256 for mbedTLS ${MBEDTLS_VERSION}." >&2
  echo "  Fetch the upstream sum and pass it explicitly:" >&2
  echo "  curl -fsSL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MBEDTLS_VERSION}/mbedtls-${MBEDTLS_VERSION}-sha256sum.txt" >&2
  echo "  MBEDTLS_SHA256=<sum> $0" >&2
  exit 1
fi

# Verify before unpacking: tar on a tampered archive is the thing being avoided.
verify_tarball() {
  local f="$1" got
  got="$(sha256sum "$f" | cut -d" " -f1)"
  if [ "$got" != "${MBEDTLS_SHA256}" ]; then
    echo "build_mbedtls: SHA-256 MISMATCH for $f" >&2
    echo "  expected ${MBEDTLS_SHA256}" >&2
    echo "  got      ${got}" >&2
    rm -f "$f"
    exit 1
  fi
  echo "build_mbedtls: sha256 ok (${got})"
}

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
    verify_tarball "${TARBALL}"
    tar -xjf "${TARBALL}"
    rm -f "${TARBALL}"
  fi
  # Thread safety: OpenChime uses mbedTLS from multiple threads — the client
  # app-core runs a network thread per connection, and the test suite drives
  # several TLS clients concurrently. Without MBEDTLS_THREADING the library's
  # internal shared state races (intermittent handshake failures). Enable the
  # pthread threading layer; the final binaries already link -lpthread. Idempotent.
  CONF="${SRC}/include/mbedtls/mbedtls_config.h"
  sed -i \
    -e 's|^//#define MBEDTLS_THREADING_C$|#define MBEDTLS_THREADING_C|' \
    -e 's|^//#define MBEDTLS_THREADING_PTHREAD$|#define MBEDTLS_THREADING_PTHREAD|' \
    "${CONF}"

  echo "build_mbedtls: building ${SRC} static libraries"
  # `make lib` builds only the libraries (skips programs/tests). The release
  # tarball ships pre-generated sources, so no Python is required.
  make -C "${SRC}" -j"$(nproc)" lib
fi

echo "build_mbedtls: done — libs in third_party/${SRC}/library, headers in third_party/${SRC}/include"
