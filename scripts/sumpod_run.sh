#!/usr/bin/env bash
# The summary trials, run on the server itself (docs/SUMMARIES.md §7):
# production-like, one model after another. For each model, the real daemon
# built for it (scripts/sumpod.sh) starts on a fresh copy of the workspace
# with summaries on, fetches and checks its model as it would in the field,
# carries on with its own background work, and is asked for every span of the
# set over the wire by a client (demo_client summarize), one at a time, as a
# person would. Development only.
#
#   sumpod_run.sh <bundle dir> <results dir> [eval|held|all] [model ...]
#
# With SUMPOD_URL set, the bundle's own openchimed runs with
# OPENCHIME_SUMMARY=cloud against that API (SUMMARIES.md §6), each model named
# as the API names it, with the key read from SUMPOD_KEY_FILE: the same
# pipeline with the model calls made elsewhere.
#
# Per model, under <results dir>/<model>/: <id>.json (the body), <id>.log (the
# client's queue positions and status), daemon.log, mem.tsv (the daemon's
# resident memory every 2 s) and measure.tsv (one line per span: id, class,
# status, seconds, start and end time, and the most memory the daemon held while
# the span was made). <results dir>/DONE appears when every model has run.
set -uo pipefail

B="$(cd "$1" && pwd)"; R="$2"; WHICH="${3:-eval}"; shift 3 2>/dev/null || shift $#
mkdir -p "$R"; R="$(cd "$R" && pwd)"
MODELS=("$@")
if [ ${#MODELS[@]} -eq 0 ]; then
    for f in "$B"/models/*.h; do MODELS+=("$(basename "$f" .h)"); done
fi
PROTO=18445; HEALTH=18082
# A span that has not ended in two hours is recorded as unfinished and the run
# moves on, so a stuck model cannot hold the server for ever.
SPAN_LIMIT=7200

for m in "${MODELS[@]}"; do
    W="$R/$m"
    [ -f "$W/measure.tsv.done" ] && continue
    rm -rf "$W"; mkdir -p "$W/db" "$W/blobs"
    cp "$B/slack.db" "$W/db/oc.db"
    ENVS=(OPENCHIME_DB_PATH="$W/db/oc.db" OPENCHIME_TLS_CERT="$W/cert.pem" OPENCHIME_TLS_KEY="$W/key.pem"
          OPENCHIME_BLOB_DIR="$W/blobs" OPENCHIME_PROTO_PORT="$PROTO" OPENCHIME_HEALTH_PORT="$HEALTH"
          OPENCHIME_TEST_PASSWORD_AUTH=1 OPENCHIME_BOOTSTRAP_USERS="alice:pw:owner")
    if [ -n "${SUMPOD_URL:-}" ]; then
        env "${ENVS[@]}" OPENCHIME_SUMMARY=cloud OPENCHIME_SUMMARY_URL="$SUMPOD_URL" OPENCHIME_SUMMARY_MODEL="$m" \
            OPENCHIME_SUMMARY_API_KEY="$(cat "$SUMPOD_KEY_FILE")" "$B/openchimed" >"$W/daemon.log" 2>&1 &
    else
        env "${ENVS[@]}" OPENCHIME_SUMMARY=local "$B/models/openchimed-$m" >"$W/daemon.log" 2>&1 &
    fi
    D=$!
    ( while kill -0 "$D" 2>/dev/null; do
          echo -e "$(date +%s)\t$(awk '/^VmRSS/{print $2}' /proc/$D/status 2>/dev/null)" >>"$W/mem.tsv"
          sleep 2
      done ) &
    S=$!
    # Up, model fetched, checked and loaded: or not, and then this model is over.
    up=""
    for _ in $(seq 1 1800); do
        grep -q "summaries on" "$W/daemon.log" && { up=1; break; }
        grep -q "summaries are off" "$W/daemon.log" && break
        kill -0 "$D" 2>/dev/null || break
        sleep 2
    done
    if [ -z "$up" ]; then
        echo "$m: summaries did not come up" >>"$R/errors.txt"
    else
        while IFS=$'\t' read -r id class set ch start end msgs words tokens; do
            case "$id" in ""|\#*) continue ;; esac
            [ "$WHICH" = all ] || [ "$set" = "$WHICH" ] || continue
            t0=$(date +%s)
            timeout "$SPAN_LIMIT" "$B/demo_client" 127.0.0.1 "$PROTO" alice pw summarize "$ch" "$start" "$end" \
                >"$W/$id.json" 2>"$W/$id.log"
            rc=$?
            t1=$(date +%s)
            st=$(sed -n 's/.*summary status \([0-9]*\) in \([0-9.]*\) s.*/\1/p' "$W/$id.log")
            secs=$(sed -n 's/.*summary status \([0-9]*\) in \([0-9.]*\) s.*/\2/p' "$W/$id.log")
            [ "$rc" = 124 ] && st=unfinished
            peak=$(awk -v a="$t0" -v b="$t1" '$1>=a && $1<=b+2 && $2>p {p=$2} END{print p+0}' "$W/mem.tsv")
            echo -e "$id\t$class\t${st:-error}\t${secs:-}\t$t0\t$t1\t$peak\t$msgs\t$words\t$tokens" >>"$W/measure.tsv"
        done <"$B/set.tsv"
    fi
    grep VmHWM /proc/$D/status >"$W/peak.txt" 2>/dev/null
    kill "$D" 2>/dev/null; wait "$D" 2>/dev/null
    kill "$S" 2>/dev/null; wait "$S" 2>/dev/null
    rm -f "$W/db/summary/"*.gguf
    touch "$W/measure.tsv.done"
done
touch "$R/DONE"
