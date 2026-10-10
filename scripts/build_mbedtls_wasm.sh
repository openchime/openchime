#!/usr/bin/env bash
# mbedTLS for the web client (docs/WEB.md): the vendored tree, copied and built
# with Emscripten into build/wasm/mbedtls, beside the native build it must not
# touch. Needs emcc on PATH (source emsdk_env.sh). Development only.
set -euo pipefail
cd "$(dirname "$0")/.."
SRC=third_party/mbedtls-3.6.2
OUT=build/wasm/mbedtls
if [ -f "$OUT/library/libmbedtls.a" ]; then echo "build_mbedtls_wasm: already built -- $OUT"; exit 0; fi
command -v emcc >/dev/null || { echo "build_mbedtls_wasm: emcc not on PATH (source ~/emsdk/emsdk_env.sh)" >&2; exit 1; }
mkdir -p build/wasm
# The vendored tree, fetched and configured by the native script when it is not there yet.
[ -d "$SRC" ] || scripts/build_mbedtls.sh fetch
rm -rf "$OUT"
rsync -a --exclude '*.o' --exclude '*.a' --exclude '*.d' "$SRC/" "$OUT/"
emmake make -C "$OUT/library" -j"$(nproc)" static CC=emcc AR=emar CFLAGS="-O2 -pthread"
echo "build_mbedtls_wasm: built $OUT/library/libmbed{tls,x509,crypto}.a"
