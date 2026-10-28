#!/usr/bin/env bash
# mbedTLS for the web client (docs/WEB.md): the vendored tree, copied and built
# with Emscripten into build/wasm/mbedtls, beside the native build it must not
# touch. Run by make (mk/web.mk), which puts the pinned Emscripten on PATH.
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=third_party/mbedtls-3.6.2
OUT=build/wasm/mbedtls
if [ -f "$OUT/library/libmbedtls.a" ]; then echo "build_mbedtls_wasm: already built -- $OUT"; exit 0; fi
command -v emcc >/dev/null || { echo "build_mbedtls_wasm: emcc not on PATH (make fetches it: mk/toolchains.mk)" >&2; exit 1; }
mkdir -p build/wasm
# The native script's lock (scripts/build/mbedtls.sh), held while the tree is
# fetched and copied: under `make -j` the native build may be fetching or
# configuring it at this moment, and a copy taken before its threading edit
# would build a wasm library without threads.
if command -v flock >/dev/null; then
  exec {lock_fd}>"third_party/.mbedtls-3.6.2.lock"
  flock "${lock_fd}"
fi
# The vendored tree, fetched and configured by the native script when it is not there yet.
[ -d "$SRC" ] || OC_MBEDTLS_LOCKED=1 scripts/build/mbedtls.sh fetch
rm -rf "$OUT"
rsync -a --exclude '*.o' --exclude '*.a' --exclude '*.d' "$SRC/" "$OUT/"
if [ -n "${lock_fd:-}" ]; then exec {lock_fd}>&-; fi
emmake make -C "$OUT/library" -j"$(nproc)" static CC=emcc AR=emar CFLAGS="-O2 -pthread"
echo "build_mbedtls_wasm: built $OUT/library/libmbed{tls,x509,crypto}.a"
