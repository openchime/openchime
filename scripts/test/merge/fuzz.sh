#!/usr/bin/env bash
# scripts/test/merge/fuzz.sh
#
# The coverage-guided fuzzing tier's short pass (docs/TESTING.md, "Fuzzing"):
# each harness under ASan and UBSan, from its checked-in corpus with the
# dictionary, for FUZZ_SECONDS (default 60), on a scratch copy of the corpus so
# nothing it grows lands in the tree. A finding fails it, naming the harness and
# the crash file; the sanitizer's report is above it in the log.
#
# Needs: build/fuzz/frame and build/fuzz/framebuf (built with a clang that has
# libFuzzer, make's FUZZ_CC).
# Run by: make test-merge (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
seconds=${FUZZ_SECONDS:-60}
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
for h in frame framebuf; do
    bin=build/fuzz/$h
    have "$bin" "make build/fuzz/$h"
    mkdir -p "$scratch/$h" "$scratch/crash-$h"
    cp tests/fuzz/corpus/$h/* "$scratch/$h/"
    echo "$TEST: $h for ${seconds}s"
    if ! "$bin" -dict=tests/fuzz/frame.dict -max_total_time="$seconds" -print_final_stats=1 \
            -artifact_prefix="$scratch/crash-$h/" "$scratch/$h" >"$scratch/$h.log" 2>&1; then
        tail -n 80 "$scratch/$h.log"
        found=$(ls "$scratch/crash-$h" 2>/dev/null | head -1)
        if [ -n "$found" ]; then
            echo "$TEST: the input, base64 (decode it into tests/fuzz/crashes/$h/ to reproduce):"
            base64 -w0 "$scratch/crash-$h/$found"; echo
        fi
        fail "$h found ${found:-a failure}"
    fi
    grep -E '^stat::(number_of_executed_units|new_units_added)' "$scratch/$h.log" || true
done
ok "frame and framebuf ran ${seconds}s each, clean"
