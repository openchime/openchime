#!/usr/bin/env bash
# Fetch the read-aloud voice model, Kitten mini v0.8 (Apache-2.0), and prepare it
# for embedding in openchimed (ARCH-111, docs/READ-ALOUD.md §4).
#
#   scripts/build_kitten.sh     -> build/kitten/{kitten.ort, voices.npz, LICENSE}
#
# The model is fetched at pinned revisions and checked by SHA-256, then converted
# to ONNX Runtime's .ort format for this machine's architecture with
# scripts/tts_convert.c, linked against the prebuilt full ONNX Runtime (a build
# tool only; scripts/build_onnxruntime.sh converter). The .ort file is not byte
# reproducible, so it is not pinned; its input and the converter are.
set -euo pipefail

HF_REV="c02725660cea441db4c383af69f1f26f5cd00947"
HF="https://huggingface.co/KittenML/kitten-tts-mini-0.8/resolve/${HF_REV}"
GH_REV="be5758500b731b8fc674acc62ea480d3022b7ebe"
GH="https://raw.githubusercontent.com/KittenML/KittenTTS/${GH_REV}"
FILES=(
  "kitten_tts_mini_v0_8.onnx|${HF}/kitten_tts_mini_v0_8.onnx|0f5bbae4fc4800c98dbc544a87ecfa79510de2fb8222db30d12e5bfe9177df91"
  "voices.npz|${HF}/voices.npz|40ad2638952b77b7b2f30127e2608e169fc69dd256b53bd8aaa3409a33193c42"
  "LICENSE|${GH}/LICENSE|787aa312b93eee80b636f35652469df29efdb7582239d6a9c088467555a3c7fd"
)

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
OUT="${ROOT}/build/kitten"
mkdir -p "${OUT}"

case "$(uname -m)" in
  x86_64)        TARGET=amd64 ;;
  aarch64|arm64) TARGET=arm ;;
  *) echo "build_kitten: unsupported architecture $(uname -m)" >&2; exit 2 ;;
esac

for entry in "${FILES[@]}"; do
  IFS='|' read -r name url want <<< "${entry}"
  out="${OUT}/${name}"
  if [ ! -f "${out}" ] || [ "$(sha256sum "${out}" | cut -d" " -f1)" != "${want}" ]; then
    echo "build_kitten: downloading ${name}"
    curl -fsSL -o "${out}.part" "${url}"
    got="$(sha256sum "${out}.part" | cut -d" " -f1)"
    if [ "${got}" != "${want}" ]; then
      echo "build_kitten: SHA-256 MISMATCH for ${name}" >&2
      echo "  expected ${want}" >&2
      echo "  got      ${got}" >&2
      rm -f "${out}.part"
      exit 1
    fi
    mv "${out}.part" "${out}"
  fi
done

stamp="$(sha256sum "${OUT}/kitten_tts_mini_v0_8.onnx" | cut -d" " -f1) ${TARGET} $(sha256sum scripts/tts_convert.c | cut -d" " -f1)"
if [ -f "${OUT}/kitten.ort" ] && [ "$(cat "${OUT}/.stamp" 2>/dev/null)" = "${stamp}" ]; then
  echo "build_kitten: up to date — build/kitten/kitten.ort"
  exit 0
fi

scripts/build_onnxruntime.sh converter
PRE="third_party/onnxruntime-1.30.0-prebuilt"
${CC:-cc} -O2 -I"${PRE}/include" scripts/tts_convert.c -L"${PRE}/lib" -lonnxruntime \
  -Wl,-rpath,"${ROOT}/${PRE}/lib" -o build/tts_convert
build/tts_convert "${OUT}/kitten_tts_mini_v0_8.onnx" "${OUT}/kitten.ort.part" "${TARGET}"
mv "${OUT}/kitten.ort.part" "${OUT}/kitten.ort"
echo "${stamp}" > "${OUT}/.stamp"

echo "build_kitten: done — build/kitten/kitten.ort ($(du -h "${OUT}/kitten.ort" | cut -f1)), voices.npz"
