#!/usr/bin/env bash
# Rebuild ttskit's data files from sources (docs/TTSKIT.md §8): the pronunciation
# dictionary and the guesser read-aloud uses (ARCH-111).
#
#   scripts/build_ttskit_data.sh           build into build/ttskit-data and compare
#   scripts/build_ttskit_data.sh install   ... and copy over ttskit/data and the test reference
#
# Run by a maintainer when the sources, the IPA table or the training settings
# change -- not by users and not by CI, which only checks the committed files
# against their pins (tests/test_ttskit.c). Needs curl, python3 (to unpack a
# wheel), a C compiler and about 1 GB of memory; takes about two minutes on
# x86-64 Linux.
#
#   1. fetch CMUdict at a pinned commit                        cmudict.dict
#   2. ARPAbet to IPA, first pronunciation of each word       lexicon.ipa, train.lex
#   3. train the guesser: align letters to phonemes, then a
#      joint n-gram model of order 6 (Phonetisaurus)           model.o6.arpa
#   4. pack both into the memory-mapped files ttskit reads     lexicon.bin, guesses.bin
#   5. decode the reference words with Phonetisaurus's own
#      decoder, the answers the C decoder is tested against   ttskit_guess_ref.txt
#
# Phonetisaurus (BSD-3), with the OpenFst (Apache-2.0) and MITLM (MIT) tools
# bundled in its wheel, is a build-time tool only: nothing of it ships, and
# OpenChime builds without it.
set -euo pipefail

MODE="${1:-check}"
case "${MODE}" in check|install) ;; *) echo "build_ttskit_data: mode is check or install, not ${MODE}" >&2; exit 2 ;; esac

CMUDICT_COMMIT="74790861f652b15e4ac49015a90074ad62a27690"
CMUDICT_SHA256="81917843c7f44ce2b094ac63873c2c7a4cf802040792c455ba3ca406891c3d22"
CMUDICT_URL="https://raw.githubusercontent.com/cmusphinx/cmudict/${CMUDICT_COMMIT}/cmudict.dict"

PHONETISAURUS_WHEEL="phonetisaurus-0.3.0-py3-none-manylinux1_x86_64.whl"
PHONETISAURUS_SHA256="4ad830d748234d778c9e55da731df319f92805120d158825cf8162616bbccc09"
PHONETISAURUS_URL="https://files.pythonhosted.org/packages/f8/dd/8bee1dc1f6944fec8a5a8a3a1e5a70e2680ef1e129c0ee946202a65ecf37/${PHONETISAURUS_WHEEL}"

ORDER=6

fetch() {
  local url="$1" out="$2" want="$3" got
  if [ ! -f "$out" ]; then
    echo "build_ttskit_data: downloading ${url}"
    curl -fsSL -o "$out.part" "$url"
    mv "$out.part" "$out"
  fi
  got="$(sha256sum "$out" | cut -d" " -f1)"
  if [ "$got" != "$want" ]; then
    echo "build_ttskit_data: SHA-256 MISMATCH for $out" >&2
    echo "  expected ${want}" >&2
    echo "  got      ${got}" >&2
    rm -f "$out"
    exit 1
  fi
}

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
OUT="${ROOT}/build/ttskit-data"
mkdir -p "${OUT}"

make -s build/tts_pack
PACK="${ROOT}/build/tts_pack"

# 1. Sources.
fetch "${CMUDICT_URL}" "${OUT}/cmudict.dict" "${CMUDICT_SHA256}"
fetch "${PHONETISAURUS_URL}" "${OUT}/${PHONETISAURUS_WHEEL}" "${PHONETISAURUS_SHA256}"
if [ ! -d "${OUT}/phonetisaurus" ]; then
  python3 -m zipfile -e "${OUT}/${PHONETISAURUS_WHEEL}" "${OUT}"        # a wheel is a zip
  chmod +x "${OUT}"/phonetisaurus/bin/x86_64/*
fi
export PATH="${OUT}/phonetisaurus/bin/x86_64:${PATH}"
export LD_LIBRARY_PATH="${OUT}/phonetisaurus/lib/x86_64${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

cd "${OUT}"

# 2. The dictionary in IPA, and the same with phonemes spaced for training.
"${PACK}" ipa cmudict.dict lexicon.ipa train.lex

# 3. The guesser. --seq2_del lets a letter map to no phoneme (the "e" in "make").
rm -rf train
echo "build_ttskit_data: training the guesser (order ${ORDER}, about two minutes)"
phonetisaurus-train --lexicon train.lex --seq2_del --ngram_order "${ORDER}" > train.log 2>&1 \
  || { cat train.log >&2; exit 1; }

# 4. The files ttskit maps.
"${PACK}" lexicon lexicon.ipa lexicon.bin
"${PACK}" guesser "train/model.o${ORDER}.arpa" guesses.bin

# 5. The reference: the same words as the committed reference, decoded by
#    Phonetisaurus's decoder, phonemes joined the way ttskit writes them.
cut -f1 "${ROOT}/tests/data/ttskit_guess_ref.txt" > ref.words
phonetisaurus-g2pfst --model=train/model.fst --wordlist=ref.words --nbest=1 2>/dev/null \
  | awk -F'\t' '{ gsub(/ /, "", $3); print $1 "\t" $3 }' > ttskit_guess_ref.txt

echo
sha256sum lexicon.bin guesses.bin
ls -l lexicon.bin guesses.bin | awk '{ print $5, $NF }'

same=1
cmp -s lexicon.bin "${ROOT}/ttskit/data/lexicon.bin" || same=0
cmp -s guesses.bin "${ROOT}/ttskit/data/guesses.bin" || same=0
cmp -s ttskit_guess_ref.txt "${ROOT}/tests/data/ttskit_guess_ref.txt" || same=0

if [ "${MODE}" = install ]; then
  cp lexicon.bin guesses.bin "${ROOT}/ttskit/data/"
  cp ttskit_guess_ref.txt "${ROOT}/tests/data/"
  echo "build_ttskit_data: installed. Update the pins in tests/test_ttskit.c and TTSKIT.md §8 to the sums above."
elif [ "${same}" = 1 ]; then
  echo "build_ttskit_data: identical to the committed files."
else
  echo "build_ttskit_data: differs from the committed files; rerun with 'install' to replace them."
  exit 1
fi
