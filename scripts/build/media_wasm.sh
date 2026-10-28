#!/usr/bin/env bash
# The media libraries for the web client (docs/WEB.md): libvpx, libopus and
# speexdsp built with Emscripten into build/wasm/{libvpx,opus,speexdsp}, from
# the same sources as natively, committed in third_party/libvpx,
# third_party/opus and third_party/speexdsp.
# Run by make (mk/web.mk), which puts the pinned Emscripten on PATH.
#
# libvpx is VP9 only and real-time only, as natively; its x86 assembly is
# nothing to wasm, so it builds the generic C paths with wasm SIMD left to the
# compiler. Threads are kept: the encoder's row threads run as Web Workers.
set -euo pipefail
cd "$(dirname "$0")/../.."
command -v emcc >/dev/null || { echo "build_media_wasm: emcc not on PATH (make fetches it: mk/toolchains.mk)" >&2; exit 1; }
mkdir -p build/wasm
J="$(nproc)"

OUT=build/wasm/opus
if [ ! -f "$OUT/lib/libopus.a" ]; then
  echo "build_media_wasm: opus"
  rm -rf "$OUT"; mkdir -p "$OUT/build"
  ( cd "$OUT/build"
    emconfigure "$OLDPWD/third_party/opus/configure" --host=wasm32-unknown-emscripten \
      --prefix="$(cd .. && pwd)" --disable-shared --enable-static --disable-doc --disable-extra-programs \
      --disable-intrinsics --disable-rtcd --disable-stack-protector CFLAGS="-O2 -pthread" >/dev/null
    emmake make -j"$J" >/dev/null && emmake make install >/dev/null )
fi

OUT=build/wasm/speexdsp
if [ ! -f "$OUT/lib/libspeexdsp.a" ]; then
  echo "build_media_wasm: speexdsp"
  rm -rf "$OUT"; mkdir -p "$OUT/build"
  ( cd "$OUT/build"
    emconfigure "$OLDPWD/third_party/speexdsp/configure" --host=wasm32-unknown-emscripten \
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
    emconfigure "$OLDPWD/third_party/libvpx/configure" --target=generic-gnu \
      --prefix="$(cd .. && pwd)" \
      --disable-vp8 --enable-vp9 --enable-realtime-only \
      --disable-examples --disable-tools --disable-docs --disable-unit-tests \
      --disable-shared --enable-static --enable-pic \
      --extra-cflags="-O2 -pthread" >/dev/null
    emmake make -j"$J" >/dev/null && emmake make install >/dev/null )
fi
echo "build_media_wasm: built build/wasm/{opus,speexdsp,libvpx}"
