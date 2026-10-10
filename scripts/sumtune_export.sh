#!/usr/bin/env bash
# Merge a LoRA adapter into its base, convert to GGUF, quantize, and print the
# five OC_SUM_MODEL_* lines that pin the result in daemon/sum_fetch.h
# (docs/TUNING.md).
#
#   scripts/sumtune_export.sh <base model> <adapter dir> <out dir> <name> [Q8_0|Q4_K_M]
#
# Needs a llama.cpp checkout with its Python requirements (convert_hf_to_gguf.py)
# and a built llama-quantize, named by LLAMACPP (default ../llama.cpp), plus
# torch, transformers and peft for the merge. The GGUF must then be published
# somewhere the daemon can fetch it (OC_SUM_MODEL_URL); the printed header
# carries its size and SHA-256. Development only.
set -euo pipefail

BASE="$1"; ADAPTER="$2"; OUT="$3"; NAME="$4"; QUANT="${5:-Q8_0}"
LLAMACPP="${LLAMACPP:-../llama.cpp}"
mkdir -p "$OUT"

python3 - "$BASE" "$ADAPTER" "$OUT/merged" <<'EOF'
import sys, torch
from transformers import AutoModelForCausalLM, AutoTokenizer
from peft import PeftModel
base, adapter, out = sys.argv[1:4]
model = AutoModelForCausalLM.from_pretrained(base, torch_dtype=torch.bfloat16)
model = PeftModel.from_pretrained(model, adapter).merge_and_unload()
model.save_pretrained(out, safe_serialization=True)
AutoTokenizer.from_pretrained(adapter).save_pretrained(out)
print("merged into", out)
EOF

python3 "$LLAMACPP/convert_hf_to_gguf.py" "$OUT/merged" --outtype bf16 --outfile "$OUT/$NAME-bf16.gguf"
"$LLAMACPP/build/bin/llama-quantize" "$OUT/$NAME-bf16.gguf" "$OUT/$NAME-$QUANT.gguf" "$QUANT"

FILE="$OUT/$NAME-$QUANT.gguf"
SHA=$(sha256sum "$FILE" | cut -c1-64)
BYTES=$(stat -c %s "$FILE")
LOWER=$(printf '%s' "$NAME-$QUANT" | tr 'A-Z' 'a-z')
cat <<EOF

$FILE
#define OC_SUM_MODEL_NAME   "$LOWER"
#define OC_SUM_MODEL_FILE   "$(basename "$FILE")"
#define OC_SUM_MODEL_URL    "https://<where it is published>/$(basename "$FILE")"
#define OC_SUM_MODEL_SHA256 "$SHA"
#define OC_SUM_MODEL_BYTES  ${BYTES}ull
EOF
