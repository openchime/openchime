#!/usr/bin/env bash
# SDL3 for the web client (docs/WEB.md): the committed tree the Windows build
# uses too (third_party/SDL3, docs/VENDORS.md §1; scripts/build/sdl3_windows.sh),
# built static with Emscripten into build/wasm/sdl3, out of tree, so the
# committed source is only read. Nothing is downloaded. Run by make
# (mk/web.mk), which puts the pinned Emscripten on PATH; needs cmake.
set -euo pipefail
# One compile at a time (docs/TESTING.md, "One build at a time"): the build below
# is given a literal one on its own command line, and no -j of a calling make
# reaches it through the environment.
unset MAKEFLAGS MFLAGS
# Emscripten compiles its own system libraries on first use, several at once
# unless told: one.
export EMCC_CORES=1

# The version committed in third_party/SDL3.
SDL3_VERSION=3.4.14

cd "$(dirname "$0")/../.."
TREE="$PWD/third_party/SDL3"
have="$(sed -n 's/^#define SDL_\(MAJOR\|MINOR\|MICRO\)_VERSION  *\([0-9]*\)$/\2/p' "${TREE}/include/SDL3/SDL_version.h" 2>/dev/null | paste -sd.)"
[ -n "${have}" ] || { echo "build_sdl3_wasm: third_party/SDL3 is missing" >&2; exit 1; }
[ "${have}" = "${SDL3_VERSION}" ] || { echo "build_sdl3_wasm: third_party/SDL3 is ${have}; this script builds ${SDL3_VERSION}" >&2; exit 1; }
OUT="$PWD/build/wasm/sdl3"
if [ -f "$OUT/lib/libSDL3.a" ]; then echo "build_sdl3_wasm: already built -- $OUT"; exit 0; fi
command -v emcc >/dev/null || { echo "build_sdl3_wasm: emcc not on PATH (make fetches it: mk/toolchains.mk)" >&2; exit 1; }
mkdir -p build/wasm/sdl3-src
cd build/wasm/sdl3-src
emcmake cmake -S "$TREE" -B cmake-build -DCMAKE_BUILD_TYPE=Release \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_TESTS=OFF \
  -DSDL_PTHREADS=ON -DCMAKE_INSTALL_PREFIX="$OUT" -DCMAKE_C_FLAGS="-pthread" > cmake.log 2>&1
cmake --build cmake-build -j1 >> cmake.log 2>&1
cmake --install cmake-build >> cmake.log 2>&1
echo "build_sdl3_wasm: built $OUT/lib/libSDL3.a"
