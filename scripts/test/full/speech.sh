#!/usr/bin/env bash
# scripts/test/full/speech.sh [binary]
#
# Read-aloud and voice input work from a binary and the data beside it: a
# render by the binary from its tts/ data, verified, and voice input hearing
# it from its stt/ data -- judged on words, with a tolerance
# (lib/heard_matches.sh), not on the exact string. With BUILD_ID set, the
# binary must also report that build. On a binary this machine cannot run, it
# says so and checks nothing.
#
# Needs: the binary (default ./openchimed) built with TTS=1 STT=1, its data.
# Run by: make test-all (PART=plain, on the full-feature daemon; PART=packages,
# on the release's binary and on the installed package).
. "$(dirname "$0")/../lib/common.sh"
bin=${1:-./openchimed}
have "$bin" "make build-all makes it"
if ! runnable "$bin"; then echo "$TEST: $bin is not for this machine ($(uname -m)); not run"; exit 0; fi
if [ -n "${BUILD_ID:-}" ]; then
    ver=$("$bin" --version)
    grep -q "^openchimed ${BUILD_ID} " <<<"$ver" || fail "$bin reports '$ver', not build ${BUILD_ID}"
fi
wav=$(mktemp --suffix=.wav)
trap 'rm -f "$wav"' EXIT
said="This build speaks."
"$bin" --tts-say expr-voice-2-f "$said" "$wav"
heard=$("$bin" --stt-hear "$wav")
echo "heard: $heard"
scripts/test/lib/heard_matches.sh "$said" "$heard" || fail "$bin did not hear what it said"
ok "$bin spoke and heard itself"
