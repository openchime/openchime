#!/usr/bin/env bash
# The media libraries for the web client (docs/WEB.md): libvpx, libopus and
# speexdsp built with Emscripten into build/wasm/{libvpx,opus,speexdsp}, from
# the same pinned sources the native scripts fetch and verify
# (scripts/build_libvpx.sh, build_opus.sh, build_speexdsp.sh -- run first for
# the sources when they are not there yet). Needs emcc on PATH.
#
# libvpx is VP9 only and real-time only, as natively; its x86 assembly is
# nothing to wasm, so it builds the generic C paths with wasm SIMD left to the
# compiler. Threads are kept: the encoder's row threads run as Web Workers.
set -euo pipefail
cd "$(dirname "$0")/.."
command -v emcc >/dev/null || { echo "build_media_wasm: emcc not on PATH (source ~/emsdk/emsdk_env.sh)" >&2; exit 1; }
LIBVPX_VERSION="${LIBVPX_VERSION:-1.17.0}"
OPUS_VERSION="${OPUS_VERSION:-1.6.1}"
SPEEXDSP_VERSION="${SPEEXDSP_VERSION:-1.2.1}"
mkdir -p build/wasm
J="$(nproc)"

# The sources: fetched and verified by the native scripts.
[ -d "third_party/libvpx-${LIBVPX_VERSION}/src" ]     || scripts/build_libvpx.sh fetch
[ -d "third_party/opus-${OPUS_VERSION}/src" ]         || scripts/build_opus.sh fetch
[ -d "third_party/speexdsp-${SPEEXDSP_VERSION}/src" ] || scripts/build_speexdsp.sh fetch

OUT=build/wasm/opus
if [ ! -f "$OUT/lib/libopus.a" ]; then
  echo "build_media_wasm: opus"
  rm -rf "$OUT"; mkdir -p "$OUT/build"
  ( cd "$OUT/build"
    emconfigure "$OLDPWD/third_party/opus-${OPUS_VERSION}/src/configure" --host=wasm32-unknown-emscripten \
      --prefix="$(cd .. && pwd)" --disable-shared --enable-static --disable-doc --disable-extra-programs \
      --disable-intrinsics --disable-rtcd --disable-stack-protector CFLAGS="-O2 -pthread" >/dev/null
    emmake make -j"$J" >/dev/null && emmake make install >/dev/null )
fi

OUT=build/wasm/speexdsp
if [ ! -f "$OUT/lib/libspeexdsp.a" ]; then
  echo "build_media_wasm: speexdsp"
  rm -rf "$OUT"; mkdir -p "$OUT/build"
  ( cd "$OUT/build"
    emconfigure "$OLDPWD/third_party/speexdsp-${SPEEXDSP_VERSION}/src/configure" --host=wasm32-unknown-emscripten \
      --prefix="$(cd .. && pwd)" --disable-shared --enable-static --disable-examples \
      --disable-sse --disable-neon CFLAGS="-O2 -pthread" >/dev/null
    emmake make -j"$J" >/dev/null && emmake make install >/dev/null )
fi

OUT=build/wasm/libvpx
if [ ! -f "$OUT/lib/libvpx.a" ]; then
  echo "build_media_wasm: libvpx"
  rm -rf "$OUT"; mkdir -p "$OUT/build"
  ( cd "$OUT/build"
    # generic-gnu: no target assembly; emconfigure supplies emcc as the compiler.
    emconfigure "$OLDPWD/third_party/libvpx-${LIBVPX_VERSION}/src/configure" --target=generic-gnu \
      --prefix="$(cd .. && pwd)" \
      --disable-vp8 --enable-vp9 --enable-realtime-only \
      --disable-examples --disable-tools --disable-docs --disable-unit-tests \
      --disable-shared --enable-static --enable-pic \
      --extra-cflags="-O2 -pthread" >/dev/null
    emmake make -j"$J" >/dev/null && emmake make install >/dev/null )
fi
echo "build_media_wasm: built build/wasm/{opus,speexdsp,libvpx}"
