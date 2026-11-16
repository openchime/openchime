#!/usr/bin/env bash
# Cross-build the committed mbedTLS (static, Windows) from third_party/mbedtls
# (docs/VENDORS.md §1) with mingw-w64 into third_party/mbedtls-<ver>-win/ — a
# separate tree from the native build so the two don't clobber each other.
# Nothing is downloaded. Gitignored output; reproduced by this script.
set -euo pipefail

# The version committed in third_party/mbedtls; the build directory carries it.
MBEDTLS_VERSION=3.6.7
CC="${MINGW_CC:-x86_64-w64-mingw32-gcc}"
AR="${MINGW_AR:-x86_64-w64-mingw32-ar}"

cd "$(dirname "$0")/../.."
cd third_party

# The committed tree, and that it is the version the build directory is named
# for (scripts/build/mbedtls.sh makes the same check).
TREE=mbedtls
have="$(sed -n 's/^#define MBEDTLS_VERSION_STRING  *"\(.*\)"$/\1/p' "${TREE}/include/mbedtls/build_info.h" 2>/dev/null || true)"
[ -n "${have}" ] || { echo "build_mbedtls_windows: third_party/mbedtls is missing" >&2; exit 1; }
[ "${have}" = "${MBEDTLS_VERSION}" ] || { echo "build_mbedtls_windows: third_party/mbedtls is ${have}; this script builds ${MBEDTLS_VERSION}" >&2; exit 1; }

SRC="mbedtls-${MBEDTLS_VERSION}-win"

# No MBEDTLS_THREADING edit here, deliberately -- scripts/build/mbedtls.sh enables
# MBEDTLS_THREADING_C + MBEDTLS_THREADING_PTHREAD and this script does not.
#
# That is not an oversight and not a divergence to reconcile. The native script's
# stated reason is its TEST SUITE, which drives several TLS clients concurrently;
# that suite is a Linux binary and never runs here. The Windows front-ends run the
# same app-core, but nothing in it shares an mbedTLS context between threads:
# the core starts threads (net.c, the call engine, the recorder, the player,
# dictation), but each TLS context is used by one thread only, and shared/tls.h
# keeps mbedtls_entropy_context and mbedtls_ctr_drbg_context as per-struct
# members, so every TLS context owns its own RNG.
#
# If that ever stops being true, the fix is NOT to copy the native flags:
# MBEDTLS_THREADING_PTHREAD is wrong for mingw, which needs MBEDTLS_THREADING_ALT
# plus an implementation of the mutex callbacks.
if [ ! -f "${SRC}/library/libmbedtls.a" ]; then
  if [ ! -d "${SRC}" ]; then
    echo "build_mbedtls_windows: copying third_party/${TREE} to third_party/${SRC}"
    # Upstream's make builds in place, so what is built is a copy: the
    # committed tree is only read. Under another name until it is whole.
    rm -rf "${SRC}.new"
    cp -a "${TREE}" "${SRC}.new"
    mv "${SRC}.new" "${SRC}"
  fi
  echo "build_mbedtls_windows: cross-building static libraries (mingw)"
  make -C "${SRC}" -j"$(nproc)" CC="${CC}" AR="${AR}" WINDOWS_BUILD=1 lib
fi

echo "build_mbedtls_windows: done: libs in third_party/${SRC}/library"
