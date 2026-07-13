#!/usr/bin/env bash
# Cross-build a pinned raylib (static, Windows) with mingw-w64 into
# third_party/raylib-install-win/ — the openblocks per-platform vendoring
# pattern. Runs on Linux/CI; produces a Windows static lib. Gitignored output.
set -euo pipefail

RAYLIB_TAG="${RAYLIB_TAG:-6.0}"
CC="${MINGW_CC:-x86_64-w64-mingw32-gcc}"
AR="${MINGW_AR:-x86_64-w64-mingw32-ar}"

cd "$(dirname "$0")/.."
mkdir -p third_party
cd third_party

SRC="raylib-${RAYLIB_TAG}-win"     # separate tree from any native raylib build
DEST="raylib-install-win"
if [ ! -f "${DEST}/lib/libraylib.a" ]; then
  if [ ! -d "${SRC}" ]; then
    echo "build_raylib_windows: cloning raylib ${RAYLIB_TAG}"
    git clone --depth 1 --branch "${RAYLIB_TAG}" https://github.com/raysan5/raylib.git "${SRC}"
  fi
  echo "build_raylib_windows: cross-building static libraylib.a (mingw)"
  make -C "${SRC}/src" -j"$(nproc)" PLATFORM=PLATFORM_DESKTOP OS=Windows_NT \
       CC="${CC}" AR="${AR}" RAYLIB_LIBTYPE=STATIC
  mkdir -p "${DEST}/lib" "${DEST}/include"
  cp "${SRC}/src/libraylib.a" "${DEST}/lib/"
  cp "${SRC}/src/raylib.h" "${SRC}/src/raymath.h" "${SRC}/src/rlgl.h" "${DEST}/include/"
fi

echo "build_raylib_windows: done — lib in third_party/${DEST}/lib"
