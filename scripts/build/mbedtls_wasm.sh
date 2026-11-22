#!/usr/bin/env bash
# mbedTLS for the web client (docs/details/WEB.md): the native build's configured copy
# of the committed tree (third_party/mbedtls, docs/details/VENDORS.md §1), copied and
# built with Emscripten into build/wasm/mbedtls, beside the native build it must
# not touch. Nothing is downloaded. Run by make (mk/web.mk), which puts the
# pinned Emscripten on PATH.
set -euo pipefail
# One compile at a time (docs/details/TESTING.md, "One build at a time"): the build below
# is given a literal one on its own command line, and no -j of a calling make
# reaches it through the environment.
unset MAKEFLAGS MFLAGS
# Emscripten compiles its own system libraries on first use, several at once
# unless told: one.
export EMCC_CORES=1
cd "$(dirname "$0")/../.."
# The version committed in third_party/mbedtls (scripts/build/mbedtls.sh).
MBEDTLS_VERSION=3.6.7
SRC="third_party/mbedtls-${MBEDTLS_VERSION}"
OUT=build/wasm/mbedtls
if [ -f "$OUT/library/libmbedtls.a" ]; then echo "build_mbedtls_wasm: already built -- $OUT"; exit 0; fi
command -v emcc >/dev/null || { echo "build_mbedtls_wasm: emcc not on PATH (make fetches it: mk/toolchains.mk)" >&2; exit 1; }
mkdir -p build/wasm
# The native script's lock (scripts/build/mbedtls.sh), held while its copy is
# made and copied again: under `make -j` the native build may be copying or
# configuring it at this moment, and a copy taken before its threading edit
# would build a wasm library without threads.
if command -v flock >/dev/null; then
  exec {lock_fd}>"third_party/.mbedtls-${MBEDTLS_VERSION}.lock"
  flock "${lock_fd}"
fi
# The configured copy, made by the native script when it is not there yet.
[ -d "$SRC" ] || OC_MBEDTLS_LOCKED=1 scripts/build/mbedtls.sh sources
rm -rf "$OUT"
rsync -a --exclude '*.o' --exclude '*.a' --exclude '*.d' "$SRC/" "$OUT/"
if [ -n "${lock_fd:-}" ]; then exec {lock_fd}>&-; fi
emmake make -C "$OUT/library" -j1 static CC=emcc AR=emar CFLAGS="-O2 -pthread"
echo "build_mbedtls_wasm: built $OUT/library/libmbed{tls,x509,crypto}.a"
