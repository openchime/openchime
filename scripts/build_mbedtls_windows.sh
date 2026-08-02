#!/usr/bin/env bash
# Cross-build the pinned mbedTLS (static, Windows) with mingw-w64 into
# third_party/mbedtls-<ver>-win/ — a separate tree from the native build so the
# two don't clobber each other. Gitignored output; reproduced by this script.
set -euo pipefail

MBEDTLS_VERSION="${MBEDTLS_VERSION:-3.6.2}"
CC="${MINGW_CC:-x86_64-w64-mingw32-gcc}"
AR="${MINGW_AR:-x86_64-w64-mingw32-ar}"


# Known-good SHA-256 of the release tarball, taken from the mbedtls-${MBEDTLS_VERSION}
# -sha256sum.txt asset upstream publishes beside it. Override both together
# when bumping MBEDTLS_VERSION -- a version without a matching sum is refused
# rather than fetched unverified, because an unpinned fetch of a TLS library
# is the one dependency where "probably fine" is not an answer.
MBEDTLS_SHA256_3_6_2="8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca"
_sum_var="MBEDTLS_SHA256_${MBEDTLS_VERSION//./_}"
MBEDTLS_SHA256="${MBEDTLS_SHA256:-${!_sum_var:-}}"
if [ -z "${MBEDTLS_SHA256}" ]; then
  echo "build_mbedtls_windows: no known SHA-256 for mbedTLS ${MBEDTLS_VERSION}." >&2
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
    echo "build_mbedtls_windows: SHA-256 MISMATCH for $f" >&2
    echo "  expected ${MBEDTLS_SHA256}" >&2
    echo "  got      ${got}" >&2
    rm -f "$f"
    exit 1
  fi
  echo "build_mbedtls_windows: sha256 ok (${got})"
}

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
    verify_tarball "${TARBALL}"
    mkdir -p "${SRC}"
    tar -xjf "${TARBALL}" -C "${SRC}" --strip-components=1
    rm -f "${TARBALL}"
  fi
  echo "build_mbedtls_windows: cross-building static libraries (mingw)"
  make -C "${SRC}" -j"$(nproc)" CC="${CC}" AR="${AR}" WINDOWS_BUILD=1 lib
fi

echo "build_mbedtls_windows: done — libs in third_party/${SRC}/library"
