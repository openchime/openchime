#!/usr/bin/env bash
# scripts/test/full/static_link.sh [binary]
#
# The daemon links only the C runtime dynamically. ONNX Runtime, llama.cpp,
# their C++ runtime, mbedTLS, libopus and SQLite are inside the binary, so
# anything else here is a dependency that crept back in.
#
# Needs: ldd; the binary (default openchimed).
# Run by: make test-all (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
need ldd "libc-bin"
bin=${1:-openchimed}
have "$bin" "make builds it"
deps=$(ldd "$bin")
echo "$deps"
extra=$(grep -vE 'linux-vdso|libc\.so|libm\.so|ld-linux' <<<"$deps" || true)
[ -z "$extra" ] || fail "$bin has a dynamic dependency beyond libc and libm: $extra"
ok "$bin links only libc and libm"
