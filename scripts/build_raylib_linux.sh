#!/usr/bin/env bash
# Build a pinned raylib (static, desktop Linux) into third_party/raylib-install-linux/
# — the per-platform vendoring pattern (mirrors build_raylib_windows.sh). The
# built lib is gitignored; this script reproduces it. Needs the desktop GL/X11
# dev headers (libgl1-mesa-dev libx11-dev libxrandr-dev libxi-dev libxcursor-dev
# libxinerama-dev).
set -euo pipefail

RAYLIB_TAG="${RAYLIB_TAG:-6.0}"

cd "$(dirname "$0")/.."
mkdir -p third_party
cd third_party

# Prefer a neutral source tree; reuse the Windows clone's tree if that's all
# that's present (same upstream tag, platform-agnostic source).
SRC="raylib-${RAYLIB_TAG}"
[ -d "${SRC}" ] || { [ -d "raylib-${RAYLIB_TAG}-win" ] && SRC="raylib-${RAYLIB_TAG}-win"; }
DEST="raylib-install-linux"

if [ ! -f "${DEST}/lib/libraylib.a" ]; then
  if [ ! -d "${SRC}" ]; then
    SRC="raylib-${RAYLIB_TAG}"
    echo "build_raylib_linux: cloning raylib ${RAYLIB_TAG}"
    git clone --depth 1 --branch "${RAYLIB_TAG}" https://github.com/raysan5/raylib.git "${SRC}"
  fi
  echo "build_raylib_linux: building static libraylib.a (PLATFORM_DESKTOP)"
  make -C "${SRC}/src" clean >/dev/null 2>&1 || true
  make -C "${SRC}/src" -j"$(nproc)" PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC
  mkdir -p "${DEST}/lib" "${DEST}/include"
  cp "${SRC}/src/libraylib.a" "${DEST}/lib/"
  cp "${SRC}/src/raylib.h" "${SRC}/src/raymath.h" "${SRC}/src/rlgl.h" "${DEST}/include/"
fi

echo "build_raylib_linux: done — lib in third_party/${DEST}/lib"
