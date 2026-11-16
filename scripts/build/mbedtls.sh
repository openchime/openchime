#!/usr/bin/env bash
# Build the committed mbedTLS (static libs) from third_party/mbedtls -- the
# release tarball as upstream published it, committed (docs/VENDORS.md §1) --
# into third_party/mbedtls-<version>[-arch]. Nothing is downloaded.
#
#   scripts/build/mbedtls.sh            the native libraries
#   scripts/build/mbedtls.sh sources    the configured copy alone, for another
#                                       toolchain's build (scripts/build/mbedtls_wasm.sh)
#
# We vendor a fixed version rather than use distro packages because those
# diverge across our build environments (Ubuntu ships mbedTLS 2.28, Alpine
# ships 3.6.x) and their APIs are not source-compatible. Building one pinned
# version from source gives local, CI, and the Docker image the same library.
# Upstream's make builds in place, so what is built is a copy: the committed
# tree is only read. The copy under third_party/ is gitignored; this script
# reproduces it.
set -euo pipefail

# The version committed in third_party/mbedtls; the build directories carry it.
MBEDTLS_VERSION=3.6.7

cd "$(dirname "$0")/../.."
cd third_party

# The committed tree, and that it is the version the build directories are
# named for: a tree replaced without this line following it is refused, not
# built under the old name.
TREE=mbedtls
have="$(sed -n 's/^#define MBEDTLS_VERSION_STRING  *"\(.*\)"$/\1/p' "${TREE}/include/mbedtls/build_info.h" 2>/dev/null || true)"
[ -n "${have}" ] || { echo "build_mbedtls: third_party/mbedtls is missing" >&2; exit 1; }
[ "${have}" = "${MBEDTLS_VERSION}" ] || { echo "build_mbedtls: third_party/mbedtls is ${have}; this script builds ${MBEDTLS_VERSION}" >&2; exit 1; }

# OC_ARCH (make ARCH=aarch64, mk/daemon.mk): a build for another architecture
# with the CC make exported, in a directory of its own beside the host's.
SRC="mbedtls-${MBEDTLS_VERSION}${OC_ARCH:+-$OC_ARCH}"

# One run at a time over the tree: under `make -j` the host's build, a cross
# build's and the wasm copy (scripts/build/mbedtls_wasm.sh, which holds this
# lock while it copies and sets OC_MBEDTLS_LOCKED for the run it makes) can
# start together, and each must see the tree copied, configured and built
# whole, not half-way. Held from the check to the end of the build.
if [ -z "${OC_MBEDTLS_LOCKED:-}" ] && command -v flock >/dev/null; then
  exec {lock_fd}>".mbedtls-${MBEDTLS_VERSION}.lock"
  flock "${lock_fd}"
fi

if [ ! -f "${SRC}/library/libmbedtls.a" ]; then
  if [ ! -d "${SRC}" ]; then
    echo "build_mbedtls: copying third_party/${TREE} to third_party/${SRC}"
    # Under another name until it is whole, so a copy cut short is never
    # taken for the tree.
    rm -rf "${SRC}.new"
    cp -a "${TREE}" "${SRC}.new"
    mv "${SRC}.new" "${SRC}"
  fi
  # Thread safety: OpenChime uses mbedTLS from multiple threads — the client
  # app-core runs a network thread per connection, and the test suite drives
  # several TLS clients concurrently. Without MBEDTLS_THREADING the library's
  # internal shared state races (intermittent handshake failures). Enable the
  # pthread threading layer; the final binaries already link -lpthread. Idempotent.
  # An edit to the copy: the committed tree stays as upstream released it.
  CONF="${SRC}/include/mbedtls/mbedtls_config.h"
  sed -i \
    -e 's|^//#define MBEDTLS_THREADING_C$|#define MBEDTLS_THREADING_C|' \
    -e 's|^//#define MBEDTLS_THREADING_PTHREAD$|#define MBEDTLS_THREADING_PTHREAD|' \
    "${CONF}"

  echo "build_mbedtls: building ${SRC} static libraries"
  # `make lib` builds only the libraries (skips programs/tests). The release
  # tree carries its generated sources, so no Python is required.
  # `sources`: the configured copy alone, for another toolchain's build
  # (scripts/build/mbedtls_wasm.sh); the native libraries are not needed there.
  if [ "${1:-}" = sources ]; then echo "build_mbedtls: sources in third_party/${SRC}"; exit 0; fi
  make -C "${SRC}" -j"$(nproc)" lib ${CC:+CC="$CC"} ${AR:+AR="$AR"}
fi

echo "build_mbedtls: done: libs in third_party/${SRC}/library, headers in third_party/${SRC}/include"
