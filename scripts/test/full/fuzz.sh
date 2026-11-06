#!/usr/bin/env bash
# scripts/test/full/fuzz.sh
#
# The coverage-guided fuzzing tier's short pass (docs/TESTING.md, "Fuzzing"):
# each harness under ASan and UBSan, from its checked-in corpus with the
# dictionary, on a scratch copy of the corpus so nothing it grows lands in the
# tree. A finding fails it, naming the harness and the crash file; the
# sanitizer's report is printed with it.
#
# The same work every run: each harness runs a fixed number of inputs
# (FUZZ_RUNS_<harness>, mk/sources.mk) from a fixed seed (FUZZ_SEED, default
# 1), one harness at a time, with address randomization off (setarch -R) and
# the corpus read once (-reload=0): either would otherwise make two runs of the
# same seed try different inputs. The counts are set from CI's measured speed so
# the pass fits inside FUZZ_SECONDS (default 15) for all the harnesses
# together. That is also a gate: each harness may take its equal share of it,
# and one stopped there is named as cut short, with the inputs it ran, so a
# pass that did less than its fixed work says so. A longer run is by hand,
# with larger counts and FUZZ_SECONDS.
#
# Needs: FUZZ_HARNESSES, "<harness>:<dictionary>:<runs> ...", the dictionary
# empty for none, which make passes from mk/sources.mk, the one list of them;
# build/fuzz/<harness> for each (built with a clang that has libFuzzer, make's
# FUZZ_CC); setarch; timeout.
# Run by: make test-all (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
need setarch "util-linux"
need timeout "coreutils"
seconds=${FUZZ_SECONDS:-15}
seed=${FUZZ_SEED:-1}
[ -n "${FUZZ_HARNESSES:-}" ] || fail "FUZZ_HARNESSES names no harness (make test-all passes it from mk/sources.mk)"
read -r -a specs <<<"$FUZZ_HARNESSES"
share_ms=$(( seconds * 1000 / ${#specs[@]} ))
share=$(printf '%d.%03d' $(( share_ms / 1000 )) $(( share_ms % 1000 )))
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

failed=() short=()
start=$(date +%s%N)
for spec in "${specs[@]}"; do
    IFS=: read -r h d runs <<<"$spec"
    bin=build/fuzz/$h
    [ -x "$bin" ] || fail "$bin is missing (make build/fuzz/$h)"
    [ -n "$runs" ] || fail "$h has no input count (FUZZ_RUNS_$h in mk/sources.mk)"
    mkdir -p "$scratch/$h" "$scratch/crash-$h"
    cp tests/fuzz/corpus/$h/* "$scratch/$h/"
    rc=0
    timeout -s INT "$share" setarch "$(uname -m)" -R \
        "$bin" ${d:+-dict=$d} -seed="$seed" -runs="$runs" -reload=0 -print_final_stats=1 \
        -artifact_prefix="$scratch/crash-$h/" "$scratch/$h" >"$scratch/$h.log" 2>&1 || rc=$?
    done_runs=$(sed -n 's/^stat::number_of_executed_units: //p' "$scratch/$h.log" || true)
    found=$(ls "$scratch/crash-$h" 2>/dev/null | head -1 || true)
    if [ -n "$found" ] || { [ "$rc" != 0 ] && [ "$rc" != 124 ]; }; then
        failed+=("$h")
        tail -n 80 "$scratch/$h.log"
        if [ -n "$found" ]; then
            echo "$TEST: $h's input, base64 (decode it into tests/fuzz/crashes/$h/ to reproduce):"
            base64 -w0 "$scratch/crash-$h/$found"; echo
        fi
    elif [ "$rc" = 124 ]; then
        short+=("$h")
        echo "$TEST: $h cut short at its ${share}s share: ${done_runs:-?} of $runs inputs, seed $seed, clean"
    else
        echo "$TEST: $h clean: ${done_runs:-?} inputs, seed $seed,$(grep -o 'DONE.*corp: [^ ]*' "$scratch/$h.log" | sed 's/^DONE *//; s/^/ /' || true)"
    fi
done
took_ms=$(( ($(date +%s%N) - start) / 1000000 ))
[ "${#failed[@]}" = 0 ] || fail "${failed[*]} found a failure"
if [ "${#short[@]}" != 0 ]; then
    msg="${short[*]} did not finish its fixed inputs inside the ${seconds}s gate"
    if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::warning::$TEST: $msg"; else echo "$TEST: warning: $msg" >&2; fi
fi
took="$(( took_ms / 1000 )).$(( took_ms % 1000 / 100 ))s"
ok "${#specs[@]} harness(es) from seed $seed, one at a time, clean, in $took$([ "${#short[@]}" = 0 ] || echo "; ${#short[@]} cut short at the gate")"
