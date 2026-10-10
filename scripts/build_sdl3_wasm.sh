#!/usr/bin/env bash
# SDL3 for the web client (docs/WEB.md): the same release the Windows build
# uses (scripts/build_sdl3_windows.sh), built static with Emscripten into
# build/wasm/sdl3. Needs emcc and cmake on PATH. Development only.
set -euo pipefail
SDL3_VERSION="${SDL3_VERSION:-3.4.14}"
SDL3_SHA256_3_4_14="30d4aa2b3037718142b32dffd4e72f917ebb6cc5227150e7bb9c45efb2153aeb"
_sum_var="SDL3_SHA256_${SDL3_VERSION//./_}"
SDL3_SHA256="${SDL3_SHA256:-${!_sum_var:-}}"
cd "$(dirname "$0")/.."
OUT="$PWD/build/wasm/sdl3"
if [ -f "$OUT/lib/libSDL3.a" ]; then echo "build_sdl3_wasm: already built -- $OUT"; exit 0; fi
command -v emcc >/dev/null || { echo "build_sdl3_wasm: emcc not on PATH (source ~/emsdk/emsdk_env.sh)" >&2; exit 1; }
mkdir -p build/wasm/sdl3-src
cd build/wasm/sdl3-src
TARBALL="SDL3-${SDL3_VERSION}.tar.gz"
if [ ! -d "SDL3-${SDL3_VERSION}" ]; then
  [ -f "$TARBALL" ] || curl -fsSL -o "$TARBALL" "https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/${TARBALL}"
  got="$(sha256sum "$TARBALL" | cut -d" " -f1)"
  [ "$got" = "$SDL3_SHA256" ] || { echo "build_sdl3_wasm: SHA-256 mismatch for $TARBALL: $got" >&2; rm -f "$TARBALL"; exit 1; }
  tar -xzf "$TARBALL"
fi
emcmake cmake -S "SDL3-${SDL3_VERSION}" -B cmake-build -DCMAKE_BUILD_TYPE=Release \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_TESTS=OFF \
  -DSDL_PTHREADS=ON -DCMAKE_INSTALL_PREFIX="$OUT" -DCMAKE_C_FLAGS="-pthread" > cmake.log 2>&1
cmake --build cmake-build -j"$(nproc)" >> cmake.log 2>&1
cmake --install cmake-build >> cmake.log 2>&1
echo "build_sdl3_wasm: built $OUT/lib/libSDL3.a"
