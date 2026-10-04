#!/usr/bin/env bash
# Build llama.cpp from pinned source as ONE static library, for the summary
# model run inside openchimed (ARCH-116) -- the "fetched at build" vendoring
# class (docs/VENDORS.md §2).
#
#   scripts/build_llamacpp.sh
#
# CPU only, no shared libraries, no OpenMP (it would add libgomp to the daemon's
# runtime dependencies, which must stay libc and libm), no network code, no
# tools. The libllama and ggml archives are merged into
# third_party/llamacpp-<v>/lib/libllamacpp.a, beside include/llama.h and the ggml
# headers, so the daemon links one archive.
#
# On x86-64 the CPU code targets x86-64-v3 (AVX2, FMA, F16C: Intel Haswell and AMD
# Excavator, 2013-2015, onwards); on arm64 it uses the ARMv8 baseline (NEON).
# Needs CMake 3.14+, a C++17 compiler and curl. A few minutes with four jobs.
# CC and CXX are honoured.
set -euo pipefail

LLAMA_VERSION="0.5.0"
LLAMA_SRC_SHA256="fef9ed754f4e031fb5c663c29260feda4ebc241abb68d64a81c0f1df5f1748e2"
LLAMA_URL="https://github.com/ggml-org/llama.cpp/archive/refs/tags/v${LLAMA_VERSION}.tar.gz"

cd "$(dirname "$0")/.."
mkdir -p third_party
cd third_party

DEST="llamacpp-${LLAMA_VERSION}"
if [ -f "${DEST}/lib/libllamacpp.a" ]; then
  echo "build_llamacpp: already built — third_party/${DEST}"
  exit 0
fi

TARBALL="llama.cpp-${LLAMA_VERSION}.tar.gz"
if [ ! -f "${TARBALL}" ] || [ "$(sha256sum "${TARBALL}" | cut -d" " -f1)" != "${LLAMA_SRC_SHA256}" ]; then
  echo "build_llamacpp: downloading ${LLAMA_URL}"
  curl -fsSL -o "${TARBALL}.part" "${LLAMA_URL}"
  mv "${TARBALL}.part" "${TARBALL}"
fi
got="$(sha256sum "${TARBALL}" | cut -d" " -f1)"
if [ "${got}" != "${LLAMA_SRC_SHA256}" ]; then
  echo "build_llamacpp: SHA-256 MISMATCH for ${TARBALL}" >&2
  echo "  expected ${LLAMA_SRC_SHA256}" >&2
  echo "  got      ${got}" >&2
  rm -f "${TARBALL}"
  exit 1
fi

SRC="${DEST}-src"
rm -rf "${SRC}" && mkdir -p "${SRC}"
tar -xzf "${TARBALL}" -C "${SRC}" --strip-components=1

CPU_FLAGS=()
case "$(uname -m)" in
  x86_64)        CPU_FLAGS=(-DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_BMI2=ON) ;;
  aarch64|arm64) CPU_FLAGS=() ;;
  *) echo "build_llamacpp: unsupported architecture $(uname -m)" >&2; exit 2 ;;
esac

JOBS="${LLAMA_JOBS:-4}"
cmake -S "${SRC}" -B "${SRC}/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DGGML_NATIVE=OFF -DGGML_BACKEND_DL=OFF -DGGML_CPU_ALL_VARIANTS=OFF \
  -DGGML_OPENMP=OFF -DGGML_LLAMAFILE=ON \
  "${CPU_FLAGS[@]}" \
  -DLLAMA_CURL=OFF -DLLAMA_OPENSSL=OFF \
  -DLLAMA_BUILD_COMMON=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
  -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_SERVER=OFF
cmake --build "${SRC}/build" --target llama -j "${JOBS}"

rm -rf "${DEST}" && mkdir -p "${DEST}/lib" "${DEST}/include"
# One archive: every object of libllama and the ggml archives it needs.
ARCHIVES=$(find "${SRC}/build" -name 'libllama.a' -o -name 'libggml*.a' | sort)
MRI="CREATE ${DEST}/lib/libllamacpp.a"
for a in ${ARCHIVES}; do MRI="${MRI}
ADDLIB ${a}"; done
printf '%s\nSAVE\nEND\n' "${MRI}" | ar -M
ranlib "${DEST}/lib/libllamacpp.a"
cp "${SRC}/include/llama.h" "${SRC}/include/llama-cpp.h" "${DEST}/include/" 2>/dev/null || cp "${SRC}/include/llama.h" "${DEST}/include/"
cp "${SRC}"/ggml/include/*.h "${DEST}/include/"
cp "${SRC}/LICENSE" "${DEST}/LICENSE"
rm -rf "${SRC}"
echo "build_llamacpp: done — third_party/${DEST}"
