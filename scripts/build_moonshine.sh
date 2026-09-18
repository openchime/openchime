#!/usr/bin/env bash
# Fetch the voice-input recognizer, Moonshine Tiny Streaming (English, MIT), for
# the data directory beside openchimed (ARCH-112, docs/VOICE-INPUT.md §6).
#
#   scripts/build_moonshine.sh     -> build/moonshine/{*.ort, tokenizer.bin,
#                                     streaming_config.json, LICENSE}
#
# Moonshine publishes the model already converted to ONNX Runtime's .ort format
# and quantized, in a dated directory on its CDN that is never overwritten, so the
# files are fetched as they are and each is checked by SHA-256. Nothing is
# converted: the same bytes serve every architecture.
set -euo pipefail

CDN="https://download.moonshine.ai/model/tiny-streaming-en/quantized_26_08_21"
GH_REV="234f60faa0eb388b01cdf7e60aca232af37aefda"
GH="https://raw.githubusercontent.com/moonshine-ai/moonshine/${GH_REV}"
FILES=(
  "frontend.model.ort|${CDN}/frontend.model.ort|5121b561417b638afce0c6c31b760e37c93cf97f80d9b0031aad1fe7b6f25d61"
  "frontend.weights.ort|${CDN}/frontend.weights.ort|217da24ac6f522ebf02da8ef288e77d1ac68d50d4a6821433182e4fbf4204bbd"
  "encoder.ort|${CDN}/encoder.ort|a8414e1a5dedf9f2093d7680601dd8a9b0433e7020260eafe0e370ead91134ca"
  "adapter.ort|${CDN}/adapter.ort|22ecc949e146c49667fda28d102d4e30749a107dc88a396292aa8f277ef1347c"
  "cross_kv.ort|${CDN}/cross_kv.ort|143a36667b8d05fd9d04e8c337b7ee121f37ef299aea6b3d82bdb3d3401950b4"
  "decoder_kv.ort|${CDN}/decoder_kv.ort|8852553f312adb6c9aa4d17418015049b30f412209ee569d336548c0044627de"
  "tokenizer.bin|${CDN}/tokenizer.bin|6884b35fd6377d4c4d32336a0bc152f36b64d1e45b6503683cdc238250a8472d"
  "streaming_config.json|${CDN}/streaming_config.json|74fe5ddebd63b17caf59e8a3b18c17547ff7bce1642050edbb1c3962674f8950"
  "LICENSE|${GH}/LICENSE|fa7d1174dd8af6a7cd280be20b80d10095ed4c19b5b20b61a7715c3ad790dc5f"
)

cd "$(dirname "$0")/.."
OUT="$(pwd)/build/moonshine"
mkdir -p "${OUT}"

for entry in "${FILES[@]}"; do
  IFS='|' read -r name url want <<< "${entry}"
  out="${OUT}/${name}"
  if [ ! -f "${out}" ] || [ "$(sha256sum "${out}" | cut -d" " -f1)" != "${want}" ]; then
    echo "build_moonshine: downloading ${name}"
    curl -fsSL -o "${out}.part" "${url}"
    got="$(sha256sum "${out}.part" | cut -d" " -f1)"
    if [ "${got}" != "${want}" ]; then
      echo "build_moonshine: SHA-256 MISMATCH for ${name}" >&2
      echo "  expected ${want}" >&2
      echo "  got      ${got}" >&2
      rm -f "${out}.part"
      exit 1
    fi
    mv "${out}.part" "${out}"
  fi
done
touch "${OUT}/.done"
echo "build_moonshine: done — build/moonshine ($(du -sh "${OUT}" | cut -f1))"
