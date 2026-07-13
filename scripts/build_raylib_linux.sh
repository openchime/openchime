#!/usr/bin/env bash
# Fetch and build a pinned raylib (static, PLATFORM_DESKTOP) into
# third_party/raylib-install/ — the openblocks vendoring pattern. Needs the
# X11/GL dev headers (provided by the client build container, Dockerfile.client).
# The build output under third_party/ is gitignored; this script reproduces it.
set -euo pipefail

RAYLIB_TAG="${RAYLIB_TAG:-6.0}"
cd "$(dirname "$0")/.."
mkdir -p third_party
cd third_party

SRC="raylib-${RAYLIB_TAG}"
if [ ! -f raylib-install/lib/libraylib.a ]; then
  if [ ! -d "${SRC}" ]; then
    echo "build_raylib: cloning raylib ${RAYLIB_TAG}"
    git clone --depth 1 --branch "${RAYLIB_TAG}" https://github.com/raysan5/raylib.git "${SRC}"
  fi
  echo "build_raylib: building static libraylib.a (PLATFORM_DESKTOP)"
  make -C "${SRC}/src" -j"$(nproc)" PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC
  mkdir -p raylib-install/lib raylib-install/include
  cp "${SRC}/src/libraylib.a" raylib-install/lib/
  cp "${SRC}/src/raylib.h" "${SRC}/src/raymath.h" "${SRC}/src/rlgl.h" raylib-install/include/
fi

echo "build_raylib: done — lib in third_party/raylib-install/lib, headers in .../include"
