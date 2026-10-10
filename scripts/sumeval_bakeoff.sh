#!/bin/sh
# Run every model over the evaluation set (sumeval_set.py), each on its own copy
# of the database and one load of the model (sumeval batch), keeping for each
# span the summary as a client gets it, the tree of every model call, and its
# measurements (calls, CPU, wall time, peak memory) in measure.tsv.
#
#   sumeval_bakeoff.sh <db> <set.tsv> <outdir> <threshold> <model.gguf>...
#
# Only the "eval" rows run; the held-out rows are for validation (§7).
set -eu
db=$1; set_tsv=$2; out=$3; threshold=$4; shift 4
here=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$out"
for model in "$@"; do
    name=$(basename "$model" .gguf)
    dir="$out/$name-c$threshold"
    mkdir -p "$dir"
    [ -f "$dir/work.db" ] || cp "$db" "$dir/work.db"
    /usr/bin/time -v "$here/build/sumeval" batch "$dir/work.db" "$model" "$set_tsv" "$dir" "$threshold" eval \
        2> "$dir/batch.err" || true
done
