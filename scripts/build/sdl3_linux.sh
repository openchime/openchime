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
#
# OC_SYSROOT=<dir> builds it against a sysroot instead of the machine's own
# headers, with make's CC (and OC_ARCH for another architecture), into
# third_party/sdl3-<ver>-<sysroot's name>/: the client's arm64 packages
# against the Ubuntu 24.04 arm64 sysroot (sysroot_ubuntu_arm64.sh), and its
# Flatpak against the Freedesktop SDK (freedesktop_sdk.sh). The development
# headers are then the sysroot's, found by pkg-config inside it; the programs
# the build runs (wayland-scanner) are still the machine's.
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

DEST="sdl3-${SDL3_VERSION}${OC_SYSROOT:+-$(basename "$OC_SYSROOT")}"

# Against a sysroot: CMake told it is cross-compiling (for the machine's own
# architecture too, so it looks for nothing outside the sysroot), and
# pkg-config told to answer from inside it.
CROSS=()
if [ -n "${OC_SYSROOT:-}" ]; then
  sysroot="$(cd .. && cd "${OC_SYSROOT}" && pwd)"   # named from the tree's root
  arch="${OC_ARCH:-$(uname -m)}"
  CROSS=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR="${arch}" -DCMAKE_C_COMPILER="${CC:-cc}"
         -DCMAKE_SYSROOT="${sysroot}" -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
         -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
         -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY)
  export PKG_CONFIG_SYSROOT_DIR="${sysroot}"
  export PKG_CONFIG_LIBDIR="${sysroot}/usr/lib/${arch}-linux-gnu/pkgconfig:${sysroot}/usr/lib/pkgconfig:${sysroot}/usr/share/pkgconfig"
fi

if [ ! -f "${DEST}/lib/libSDL3.a" ]; then
  echo "build_sdl3_linux: building static SDL3 (X11 and Wayland)${OC_SYSROOT:+ against ${OC_SYSROOT}}"
  mkdir -p "${DEST}"
  cmake -S "${TREE}" -B "${DEST}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_X11_XTEST=OFF "${CROSS[@]}" \
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
