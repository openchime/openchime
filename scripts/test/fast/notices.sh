#!/usr/bin/env bash
# Every third-party component the daemon's build links has its licence notice
# in packaging/licenses.sh, for each combination of TTS, STT and SUM. The
# components are read from the build itself -- the compile and link commands
# `make -n -B openchimed` prints -- and the notices from licenses.sh's own list
# mode (LICENSES_LIST=1), so neither side is a second copy that could drift.
# Two libraries once shipped without their notices; this is what would have
# said so. Run by scripts/test/fast/source.sh with the other source checks.
#
# Compared by directory under third_party/, without an architecture suffix
# (-amd64, -arm64) or a version suffix: the build links third_party/opus-1.6.1,
# built from the source tree third_party/opus that carries the notice. Only the
# daemon's own commands count: the web client it embeds is built by emcc into
# build/wasm and is a client (its components are the clients' to declare), and
# data outside third_party/ (the speech models, CMUdict) is not a linked library.
set -uo pipefail
cd "$(dirname "$0")/../../.."
# Not the calling make's flags or variables: this asks a fresh one. And not the
# tree's remembered switches either: make remakes an included makefile even
# under -n, so a dry run with TTS=1 would rewrite build/.features and the next
# `make openchimed` would build ONNX Runtime. REMEMBER=0 reads and writes none.
unset MAKEFLAGS MFLAGS MAKELEVEL
mbedtls=$(make -s -p -n -f Makefile REMEMBER=0 FORCE 2>/dev/null | sed -n 's/^MBEDTLS_DIR := //p' | head -1)
[ -n "$mbedtls" ] || { echo "notices: could not read MBEDTLS_DIR from the Makefile" >&2; exit 1; }
# third_party/<dir> names, one per line, without an architecture or version suffix.
names() { grep -oE 'third_party/[A-Za-z0-9._+-]+' | sed -E 's|^third_party/||; s/-(amd64|arm64)$//; s/-[0-9][0-9.]*$//' | sort -u; }
fail=0
for tts in 0 1; do for stt in 0 1; do for sum in 0 1; do
    combo="TTS=$tts STT=$stt SUM=$sum"
    out=$(make --no-print-directory -n -B openchimed REMEMBER=0 TTS=$tts STT=$stt SUM=$sum 2>&1) || {
        echo "notices: make -n openchimed $combo failed:" >&2; echo "$out" | tail -5 | sed 's/^/  /' >&2; fail=1; continue; }
    built=$(echo "$out" | grep -v -e 'emscripten/emcc' -e 'build[^ ]*/wasm/' -e 'emsdk' | names)
    noticed=$(LICENSES_LIST=1 TTS=$tts STT=$stt SUM=$sum packaging/licenses.sh "$mbedtls" | names)
    missing=$(comm -23 <(echo "$built") <(echo "$noticed"))
    if [ -n "$missing" ]; then
        for c in $missing; do echo "notices: $combo: the daemon links third_party/$c and packaging/licenses.sh has no notice for it" >&2; done
        fail=1
    fi
done; done; done
[ "$fail" = 0 ] && echo "notices: every linked component has its notice, in all eight builds"
[ "$fail" = 1 ] && echo "notices: add an emit line for each to packaging/licenses.sh, under the same TTS/STT/SUM condition the build uses (CONTRIBUTING.md)" >&2
exit "$fail"
