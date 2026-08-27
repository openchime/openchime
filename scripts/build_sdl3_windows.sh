#!/usr/bin/env bash
# Cross-build the pinned SDL3 (static, Windows) with mingw-w64 into
# third_party/sdl3-<ver>-win/ — the "fetched at build" vendoring class
# (docs/VENDORS.md §2), same shape as build_mbedtls_windows.sh: SHA-256-pinned
# tarball, gitignored output, reproduced by this script. SDL's own build is
# CMake; invoking it here is the same arrangement as invoking mbedTLS's make —
# the dependency's build system stays the dependency's business, and this
# tree's stays make.
set -euo pipefail

SDL3_VERSION="${SDL3_VERSION:-3.4.14}"

# Known-good SHA-256 of the release tarball. Override both together when
# bumping SDL3_VERSION — a version without a matching sum is refused rather
# than fetched unverified. SDL is not a TLS library, but it is a quarter of a
# million lines linked into the shipped client, which is reason enough.
SDL3_SHA256_3_4_14="30d4aa2b3037718142b32dffd4e72f917ebb6cc5227150e7bb9c45efb2153aeb"
_sum_var="SDL3_SHA256_${SDL3_VERSION//./_}"
SDL3_SHA256="${SDL3_SHA256:-${!_sum_var:-}}"
if [ -z "${SDL3_SHA256}" ]; then
  echo "build_sdl3_windows: no known SHA-256 for SDL3 ${SDL3_VERSION}." >&2
  echo "  Fetch the upstream sum and pass it explicitly:" >&2
  echo "  SDL3_SHA256=<sum> $0" >&2
  exit 1
fi

verify_tarball() {
  local f="$1" got
  got="$(sha256sum "$f" | cut -d" " -f1)"
  if [ "$got" != "${SDL3_SHA256}" ]; then
    echo "build_sdl3_windows: SHA-256 MISMATCH for $f" >&2
    echo "  expected ${SDL3_SHA256}" >&2
    echo "  got      ${got}" >&2
    rm -f "$f"
    exit 1
  fi
  echo "build_sdl3_windows: sha256 ok (${got})"
}

cd "$(dirname "$0")/.."
mkdir -p third_party
cd third_party

DEST="sdl3-${SDL3_VERSION}-win"
SRC="${DEST}/src"
TARBALL="SDL3-${SDL3_VERSION}.tar.gz"
URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/${TARBALL}"

if [ ! -f "${DEST}/lib/libSDL3.a" ]; then
  if [ ! -d "${SRC}" ]; then
    echo "build_sdl3_windows: downloading SDL3 ${SDL3_VERSION}"
    curl -fsSL -o "${TARBALL}" "${URL}"
    verify_tarball "${TARBALL}"
    mkdir -p "${SRC}"
    tar -xzf "${TARBALL}" -C "${SRC}" --strip-components=1
    rm -f "${TARBALL}"
  fi
  echo "build_sdl3_windows: cross-building static SDL3 (mingw)"
  # Static only — the client links everything statically (no runtime DLLs
  # beyond the OS, same posture as the rest of the .exe). The test library is
  # SDL's own test framework, not needed.
  #
  # Our own toolchain file rather than the one SDL ships, so the pin plus
  # this script fully describe the build. C++ is not optional: SDL's Windows
  # branch does enable_language(CXX) unconditionally, and a toolchain without
  # a cross g++ makes CMake fall back to the HOST c++ — which links exactly
  # one broken try-compile before failing. CI installs g++-mingw-w64-x86-64
  # for this script alone.
  cat > "${DEST}/toolchain.cmake" <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
EOF
  cmake -S "${SRC}" -B "${DEST}/build" \
    -DCMAKE_TOOLCHAIN_FILE="$(pwd)/${DEST}/toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF \
    -DCMAKE_INSTALL_PREFIX="$(pwd)/${DEST}" >/dev/null
  cmake --build "${DEST}/build" -j"$(nproc)" >/dev/null
  cmake --install "${DEST}/build" >/dev/null
fi

echo "build_sdl3_windows: done — lib in third_party/${DEST}/lib, headers in third_party/${DEST}/include"
