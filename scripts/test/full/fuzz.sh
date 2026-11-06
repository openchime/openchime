#!/usr/bin/env bash
# scripts/test/full/fuzz.sh
#
# The coverage-guided fuzzing tier's short pass (docs/TESTING.md, "Fuzzing"):
# each harness under ASan and UBSan, from its checked-in corpus with the
# dictionary, for FUZZ_SECONDS (default 60), on a scratch copy of the corpus so
# nothing it grows lands in the tree. A finding fails it, naming the harness and
# the crash file; the sanitizer's report is above it in the log.
#
# Needs: FUZZ_HARNESSES, "<harness>:<dictionary> ...", the dictionary empty
# for none, which make passes from mk/sources.mk, the one list of them; and
# build/fuzz/<harness> for each (built with a clang that has libFuzzer, make's
# FUZZ_CC).
# Run by: make test-all (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
seconds=${FUZZ_SECONDS:-60}
[ -n "${FUZZ_HARNESSES:-}" ] || fail "FUZZ_HARNESSES names no harness (make test-all passes it from mk/sources.mk)"
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
harnesses=""
for hd in $FUZZ_HARNESSES; do
    h=${hd%%:*} d=${hd#*:}
    harnesses="$harnesses $h"
    bin=build/fuzz/$h
    have "$bin" "make build/fuzz/$h"
    mkdir -p "$scratch/$h" "$scratch/crash-$h"
    cp tests/fuzz/corpus/$h/* "$scratch/$h/"
    echo "$TEST: $h for ${seconds}s"
    if ! "$bin" ${d:+-dict=$d} -max_total_time="$seconds" -print_final_stats=1 \
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
ok "$(wc -w <<<"$harnesses") harnesses ran ${seconds}s each, clean"
