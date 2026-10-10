#!/usr/bin/env bash
# The summary trials' bundle (docs/SUMMARIES.md §7): everything a production-
# sized server needs to run scripts/sumpod_run.sh, one model after another.
# Development only; the workspace data never enters the repository.
#
#   scripts/sumpod.sh <workspace db> <set.tsv> <out dir> <model header> ...
#
# A model header pins one model, the five OC_SUM_MODEL_* names of
# daemon/sum_fetch.h; for each, a daemon is built with it (-include) as
# openchimed-<header name>. The set (scripts/sumeval_set.py) names channels;
# the bundle's names them by id, as a client asks. Then copy <out dir> to the
# server and run `sumpod_run.sh <bundle> <results>` there.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DB="$1"; SET="$2"; OUT="$3"; shift 3
mkdir -p "$OUT/models"
for h in "$@"; do
    m="$(basename "$h" .h)"
    cp "$h" "$OUT/models/$m.h"
    rm -f "$ROOT/openchimed"
    make -C "$ROOT" openchimed CFLAGS="-std=c99 -D_GNU_SOURCE -O2 -include $(cd "$(dirname "$h")" && pwd)/$(basename "$h")" >/dev/null
    cp "$ROOT/openchimed" "$OUT/models/openchimed-$m"
done
rm -f "$ROOT/openchimed"
make -C "$ROOT" openchimed demo-client >/dev/null
cp "$ROOT/build/demo_client" "$ROOT/scripts/sumpod_run.sh" "$OUT/"
cp "$DB" "$OUT/slack.db"
python3 -I - "$DB" "$SET" "$OUT/set.tsv" <<'EOF'
import sqlite3, sys
ids = {n: i for i, n in sqlite3.connect(sys.argv[1]).execute("SELECT id, name FROM channels")}
with open(sys.argv[3], "w") as out:
    for line in open(sys.argv[2]):
        f = line.rstrip("\n").split("\t")
        if not line.startswith("#"):
            f[3] = str(ids[f[3]])
        out.write("\t".join(f) + "\n")
EOF
echo "bundle in $OUT"
