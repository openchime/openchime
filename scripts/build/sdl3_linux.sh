#!/usr/bin/env bash
# Build the committed SDL3 (static, Linux) from third_party/SDL3
# (docs/details/VENDORS.md §1) natively into third_party/sdl3-<ver>/, for the
# Linux GUI (mk/linux.mk, `make linux-gui`). The sibling of sdl3_windows.sh:
# nothing is downloaded, SDL's own CMake build is invoked as the dependency's
# business, and it builds out of tree, so the committed source is only read.
# Gitignored output; reproduced by this script.
#
# Both video backends, Wayland first and X11 beside it (ARCH-80): SDL picks
# whichever the session offers at run time. Their client libraries are opened
# by SDL when it starts (SDL's *_SHARED options, on by default), so the binary
# links neither and runs under either display server; their development headers
# are needed only here, and the build fails if either backend is left out.
# XTest is not among them: SDL uses it only to warp the pointer, which this
# client never does.
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
[ -n "${have}" ] || { echo "build_sdl3_linux: third_party/SDL3 is missing" >&2; exit 1; }
[ "${have}" = "${SDL3_VERSION}" ] || { echo "build_sdl3_linux: third_party/SDL3 is ${have}; this script builds ${SDL3_VERSION}" >&2; exit 1; }
command -v cmake >/dev/null || { echo "build_sdl3_linux: cmake is not installed" >&2; exit 1; }

DEST="sdl3-${SDL3_VERSION}"

if [ ! -f "${DEST}/lib/libSDL3.a" ]; then
  echo "build_sdl3_linux: building static SDL3 (X11 and Wayland)"
  mkdir -p "${DEST}"
  cmake -S "${TREE}" -B "${DEST}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_X11_XTEST=OFF \
    -DCMAKE_INSTALL_PREFIX="$(pwd)/${DEST}" > "${DEST}/cmake.log" 2>&1 \
    || { echo "build_sdl3_linux: configuring failed; see third_party/${DEST}/cmake.log" >&2; exit 1; }
  # SDL configures without a backend whose headers it cannot find; this build
  # must have both, so a missing one stops it here rather than shipping a
  # client that cannot open a window on half the desktops.
  cfg="${DEST}/build/include-config-release/build_config/SDL_build_config.h"
  [ -f "${cfg}" ] || cfg="$(find "${DEST}/build" -name SDL_build_config.h | head -n 1)"
  for drv in WAYLAND X11; do
    grep -q "^#define SDL_VIDEO_DRIVER_${drv} 1" "${cfg}" || {
      echo "build_sdl3_linux: SDL3 configured without its ${drv} video backend: install its development headers (docs/details/DEVELOPMENT.md, "The Linux GUI")" >&2
      rm -rf "${DEST}/build"
      exit 1
    }
  done
  cmake --build "${DEST}/build" -j1 >> "${DEST}/cmake.log" 2>&1 \
    || { echo "build_sdl3_linux: building failed; see third_party/${DEST}/cmake.log" >&2; exit 1; }
  cmake --install "${DEST}/build" >> "${DEST}/cmake.log" 2>&1
fi

echo "build_sdl3_linux: done: lib in third_party/${DEST}/lib, headers in third_party/${DEST}/include"
