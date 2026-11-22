#!/usr/bin/env bash
# Cross-build the committed SDL3 (static, Windows) from third_party/SDL3
# (docs/details/VENDORS.md §1) with mingw-w64 into third_party/sdl3-<ver>-win/.
# Nothing is downloaded. SDL's own build is CMake; invoking it here is the same
# arrangement as invoking mbedTLS's make -- the dependency's build system stays
# the dependency's business, and this tree's stays make. It builds out of tree,
# so the committed source is only read. Gitignored output; reproduced by this
# script.
set -euo pipefail
# One compile at a time (docs/details/TESTING.md, "One build at a time"): the build below
# is given a literal one on its own command line, and no -j of a calling make
# reaches it through the environment.
unset MAKEFLAGS MFLAGS

# The version committed in third_party/SDL3; the build directory carries it.
SDL3_VERSION=3.4.14

cd "$(dirname "$0")/../.."
mkdir -p third_party
cd third_party

# The committed tree, and that it is the version the build directory is named
# for (scripts/build/sdl3_wasm.sh makes the same check).
TREE=SDL3
have="$(sed -n 's/^#define SDL_\(MAJOR\|MINOR\|MICRO\)_VERSION  *\([0-9]*\)$/\2/p' "${TREE}/include/SDL3/SDL_version.h" 2>/dev/null | paste -sd.)"
[ -n "${have}" ] || { echo "build_sdl3_windows: third_party/SDL3 is missing" >&2; exit 1; }
[ "${have}" = "${SDL3_VERSION}" ] || { echo "build_sdl3_windows: third_party/SDL3 is ${have}; this script builds ${SDL3_VERSION}" >&2; exit 1; }

DEST="sdl3-${SDL3_VERSION}-win"

if [ ! -f "${DEST}/lib/libSDL3.a" ]; then
  echo "build_sdl3_windows: cross-building static SDL3 (mingw)"
  mkdir -p "${DEST}"
  cat > "${DEST}/toolchain.cmake" <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
EOF
  cmake -S "${TREE}" -B "${DEST}/build" \
    -DCMAKE_TOOLCHAIN_FILE="$(pwd)/${DEST}/toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DCMAKE_INSTALL_PREFIX="$(pwd)/${DEST}" >/dev/null
  cmake --build "${DEST}/build" -j1 >/dev/null
  cmake --install "${DEST}/build" >/dev/null
fi

echo "build_sdl3_windows: done: lib in third_party/${DEST}/lib, headers in third_party/${DEST}/include"
