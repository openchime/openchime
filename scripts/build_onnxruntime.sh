#!/usr/bin/env bash
# Build ONNX Runtime from pinned source as ONE minimal static library, for the
# read-aloud voice model linked into openchimed (ARCH-111) -- the "fetched at
# build" vendoring class (docs/VENDORS.md §2).
#
#   scripts/build_onnxruntime.sh           the static library, for linking
#   scripts/build_onnxruntime.sh converter the prebuilt full library too, which
#                                          only the build uses to convert the model
#
# Minimal means: only the operator kernels (and types) Kitten uses
# (daemon/tts_kitten.ops.config), the compact .ort model format only (no protobuf
# model parser at run time), no traditional-ML operators, no exceptions, no GPU
# providers, no shared library. The dozens of static archives the build makes are
# merged into third_party/onnxruntime-<v>/lib/libonnxruntime.a, beside the C API
# headers, so the daemon links one archive.
#
# Needs CMake 3.28+, a C++17 compiler, python3 and curl. Its own dependencies
# (abseil, flatbuffers, protobuf-lite, Eigen, ...) are downloaded by its CMake at
# the URLs and hashes pinned in its cmake/deps.txt. About 22 minutes with three
# jobs on an i5-1335U; peak memory is roughly 1 GB per job, so ORT_JOBS defaults to
# what the machine's memory allows. CC and CXX are honoured.
set -euo pipefail

MODE="${1:-static}"
ORT_VERSION="1.30.0"

# The source release tarball, and the flatbuffers Python package ORT's build script
# uses to read the operator config (build-time only, unpacked, not installed).
ORT_SRC_SHA256="f6681ecbddf53898adf0cc9e8e9e84657485b84d2eca3c8aa353de6d7dd417ef"
FLATBUFFERS_WHEEL="flatbuffers-25.12.19-py2.py3-none-any.whl"
FLATBUFFERS_SHA256="7634f50c427838bb021c2d66a3d1168e9d199b0607e6329399f04846d42e20b4"
FLATBUFFERS_URL="https://files.pythonhosted.org/packages/e8/2d/d2a548598be01649e2d46231d151a6c56d10b964d94043a335ae56ea2d92/${FLATBUFFERS_WHEEL}"

# The prebuilt full release, for the converter: SHA-256s as GitHub records them.
ORT_PREBUILT_SHA256_x64="a5ed5a3cac51fbb2e90da632ae43d19212faaa20e76484e62bcb7c23ddb3b3fd"
ORT_PREBUILT_SHA256_aarch64="e16a27a8ed330bbc698df7330b0cf56e722f354e3bcc92118682c74ef3c3e3da"

fetch() {
  local url="$1" out="$2" want="$3" got
  if [ ! -f "$out" ] || [ "$(sha256sum "$out" | cut -d" " -f1)" != "$want" ]; then
    echo "build_onnxruntime: downloading ${url}"
    curl -fsSL -o "$out.part" "$url"
    mv "$out.part" "$out"
  fi
  got="$(sha256sum "$out" | cut -d" " -f1)"
  if [ "$got" != "$want" ]; then
    echo "build_onnxruntime: SHA-256 MISMATCH for $out" >&2
    echo "  expected ${want}" >&2
    echo "  got      ${got}" >&2
    rm -f "$out"
    exit 1
  fi
}

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
mkdir -p third_party
cd third_party

case "$(uname -m)" in
  x86_64)        ARCH=x64 ;;
  aarch64|arm64) ARCH=aarch64 ;;
  *) echo "build_onnxruntime: unsupported architecture $(uname -m)" >&2; exit 2 ;;
esac

# --- the prebuilt full library (converter only) -------------------------------
if [ "${MODE}" = converter ]; then
  DEST="onnxruntime-${ORT_VERSION}-prebuilt"
  if [ ! -f "${DEST}/lib/libonnxruntime.so.1" ]; then
    sum_var="ORT_PREBUILT_SHA256_${ARCH}"
    TARBALL="onnxruntime-linux-${ARCH}-${ORT_VERSION}.tgz"
    fetch "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${TARBALL}" "${TARBALL}" "${!sum_var}"
    rm -rf "${DEST}" && mkdir -p "${DEST}"
    tar -xzf "${TARBALL}" -C "${DEST}" --strip-components=1
    rm -f "${TARBALL}"
  fi
  echo "build_onnxruntime: done — third_party/${DEST}"
  exit 0
fi
[ "${MODE}" = static ] || { echo "build_onnxruntime: mode is static or converter, not ${MODE}" >&2; exit 2; }

# --- the minimal static library -----------------------------------------------
DEST="onnxruntime-${ORT_VERSION}"
OPS="${ROOT}/daemon/tts_kitten.ops.config"
STAMP="$(sha256sum "${OPS}" | cut -d" " -f1) ${CC:-cc} ${CXX:-c++}"
if [ -f "${DEST}/lib/libonnxruntime.a" ] && [ "$(cat "${DEST}/.stamp" 2>/dev/null)" = "${STAMP}" ]; then
  echo "build_onnxruntime: up to date — third_party/${DEST}/lib/libonnxruntime.a"
  exit 0
fi

WORK="onnxruntime-${ORT_VERSION}-build"
mkdir -p "${WORK}"
fetch "https://github.com/microsoft/onnxruntime/archive/refs/tags/v${ORT_VERSION}.tar.gz" "${WORK}/src.tar.gz" "${ORT_SRC_SHA256}"
fetch "${FLATBUFFERS_URL}" "${WORK}/${FLATBUFFERS_WHEEL}" "${FLATBUFFERS_SHA256}"
if [ ! -f "${WORK}/src/build.sh" ]; then
  rm -rf "${WORK}/src" && mkdir -p "${WORK}/src"
  tar -xzf "${WORK}/src.tar.gz" -C "${WORK}/src" --strip-components=1
fi
if [ ! -d "${WORK}/py/flatbuffers" ]; then
  python3 -m zipfile -e "${WORK}/${FLATBUFFERS_WHEEL}" "${WORK}/py"
fi

if [ -z "${ORT_JOBS:-}" ]; then
  mem_gb=$(awk '/MemTotal/ { printf "%d", $2 / 1048576 }' /proc/meminfo)
  ORT_JOBS=$(( mem_gb > 1 ? mem_gb - 1 : 1 ))
  [ "${ORT_JOBS}" -le "$(nproc)" ] || ORT_JOBS="$(nproc)"
fi

echo "build_onnxruntime: building ONNX Runtime ${ORT_VERSION} (minimal, static) with ${ORT_JOBS} jobs"
(
  cd "${WORK}/src"
  PYTHONPATH="$(cd ../py && pwd)" python3 tools/ci_build/build.py \
    --build_dir "$(cd .. && pwd)/out" --config MinSizeRel --update --build --parallel "${ORT_JOBS}" \
    --skip_tests --skip_pip_install --skip_submodule_sync --compile_no_warning_as_error \
    --cmake_generator "Unix Makefiles" \
    --minimal_build --include_ops_by_config "${OPS}" --enable_reduced_operator_type_support \
    --disable_ml_ops --disable_exceptions \
    --cmake_extra_defines onnxruntime_BUILD_UNIT_TESTS=OFF CMAKE_POSITION_INDEPENDENT_CODE=ON
) > "${WORK}/build.log" 2>&1 || { tail -40 "${WORK}/build.log" >&2; echo "build_onnxruntime: build failed; full log ${WORK}/build.log" >&2; exit 1; }

OUT="${WORK}/out/MinSizeRel"
rm -rf "${DEST}.new" && mkdir -p "${DEST}.new/lib" "${DEST}.new/include"
# Every archive the session needs, merged: ORT's own, the ONNX schema, protobuf-lite
# with its UTF-8 checker, cpuinfo, abseil and, on arm64, KleidiAI. protoc and full
# protobuf are build tools and stay out.
{
  echo "CREATE ${DEST}.new/lib/libonnxruntime.a"
  for a in "${OUT}"/libonnxruntime_*.a \
           "${OUT}"/_deps/onnx-build/libonnx.a "${OUT}"/_deps/onnx-build/libonnx_proto.a \
           "${OUT}"/_deps/protobuf-build/libprotobuf-lite.a \
           "${OUT}"/_deps/protobuf-build/third_party/utf8_range/libutf8_range.a \
           "${OUT}"/_deps/protobuf-build/third_party/utf8_range/libutf8_validity.a \
           "${OUT}"/_deps/pytorch_cpuinfo-build/libcpuinfo.a \
           $(find "${OUT}/_deps/abseil_cpp-build" -name '*.a' | sort); do
    [ -f "$a" ] || { echo "build_onnxruntime: expected archive missing: $a" >&2; exit 1; }
    echo "ADDLIB $a"
  done
  # KleidiAI, Arm's kernels, which ONNX Runtime builds and calls on arm64 only.
  if [ "${ARCH}" = aarch64 ]; then
    a="${OUT}/_deps/kleidiai-build/libkleidiai.a"
    [ -f "$a" ] || { echo "build_onnxruntime: expected archive missing: $a" >&2; exit 1; }
    echo "ADDLIB $a"
  fi
  echo "SAVE"
  echo "END"
} | ar -M
ranlib "${DEST}.new/lib/libonnxruntime.a"
cp "${WORK}"/src/include/onnxruntime/core/session/*.h "${DEST}.new/include/"
cp "${WORK}/src/LICENSE" "${WORK}/src/ThirdPartyNotices.txt" "${DEST}.new/"
echo "${STAMP}" > "${DEST}.new/.stamp"
rm -rf "${DEST}" && mv "${DEST}.new" "${DEST}"

echo "build_onnxruntime: done — third_party/${DEST}/lib/libonnxruntime.a ($(du -h "${DEST}/lib/libonnxruntime.a" | cut -f1))"
