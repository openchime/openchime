#!/usr/bin/env bash
# scripts/test/full/fuzz.sh
#
# The coverage-guided fuzzing tier's short pass (docs/TESTING.md, "Fuzzing"):
# each harness under ASan and UBSan, from its checked-in corpus with the
# dictionary, for FUZZ_SECONDS (default 60), on a scratch copy of the corpus so
# nothing it grows lands in the tree. A finding fails it, naming the harness and
# the crash file; the sanitizer's report is printed with it.
#
# The harnesses are independent processes with a fixed time each, so up to
# FUZZ_JOBS of them (default: one per CPU) run at once: each still fuzzes for its
# full FUZZ_SECONDS, and the pass takes that time per round of FUZZ_JOBS rather
# than per harness. Results are reported in the list's order once all are done;
# one failing fails the pass, after every harness has finished.
#
# Needs: FUZZ_HARNESSES, "<harness>:<dictionary> ...", the dictionary empty
# for none, which make passes from mk/sources.mk, the one list of them; and
# build/fuzz/<harness> for each (built with a clang that has libFuzzer, make's
# FUZZ_CC).
# Run by: make test-all (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
seconds=${FUZZ_SECONDS:-60}
jobs=${FUZZ_JOBS:-$(nproc 2>/dev/null || echo 2)}
[ -n "${FUZZ_HARNESSES:-}" ] || fail "FUZZ_HARNESSES names no harness (make test-all passes it from mk/sources.mk)"
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

# One harness, start to finish; its exit status is its verdict, its output its log.
fuzz_one() {
    local h=$1 d=$2 bin=build/fuzz/$1
    [ -x "$bin" ] || { echo "$TEST: $bin is missing (make build/fuzz/$h)"; return 1; }
    mkdir -p "$scratch/$h" "$scratch/crash-$h"
    cp tests/fuzz/corpus/$h/* "$scratch/$h/"
    "$bin" ${d:+-dict=$d} -max_total_time="$seconds" -print_final_stats=1 \
        -artifact_prefix="$scratch/crash-$h/" "$scratch/$h"
}

harnesses=()
for hd in $FUZZ_HARNESSES; do
    h=${hd%%:*} d=${hd#*:}
    harnesses+=("$h")
    while [ "$(jobs -rp | wc -l)" -ge "$jobs" ]; do wait -n || true; done
    echo "$TEST: $h for ${seconds}s"
    ( fuzz_one "$h" "$d" >"$scratch/$h.log" 2>&1; echo $? >"$scratch/$h.status" ) &
done
wait

failed=()
for h in "${harnesses[@]}"; do
    if [ "$(cat "$scratch/$h.status" 2>/dev/null)" = 0 ]; then
        echo "$TEST: $h clean"
        grep -E '^stat::(number_of_executed_units|new_units_added)' "$scratch/$h.log" || true
        continue
    fi
    failed+=("$h")
    tail -n 80 "$scratch/$h.log"
    found=$(ls "$scratch/crash-$h" 2>/dev/null | head -1 || true)
    if [ -n "$found" ]; then
        echo "$TEST: $h's input, base64 (decode it into tests/fuzz/crashes/$h/ to reproduce):"
        base64 -w0 "$scratch/crash-$h/$found"; echo
    fi
done
[ "${#failed[@]}" = 0 ] || fail "${failed[*]} found a failure"
ok "${#harnesses[@]} harnesses ran ${seconds}s each, ${jobs} at a time, clean"
