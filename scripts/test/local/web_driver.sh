#!/usr/bin/env bash
# scripts/test/local/web_driver.sh
#
# The web harness keeps exactly one driver per client directory
# (scripts/web/drive.sh): `kill` ends every driver serving it, even one that
# ignores SIGTERM, and `held` -- what `launch` checks before it starts its own
# -- says when one still does. Two drivers on one directory race for each
# command, and a run then fails with nothing wrong in the client.
#
# Driven with stand-ins, not node and Chrome: a process whose command line
# names `scripts/web/drive.mjs <dir>` as a driver's does, with SIGTERM ignored.
#
# Needs: bash, pgrep.
# Run by: make test (scripts/test/local/source.sh).
. "$(dirname "$0")/../lib/common.sh"
need pgrep procps
base=$(mktemp -d)
trap 'pkill -9 -f "scripts/web/drive.mjs $base/" 2>/dev/null || true; rm -rf "$base"' EXIT
drive() { OC_TEST_DIR="$base" OC_TEST_WHO=a scripts/web/drive.sh "$@"; }
dir="$base/web-a"
mkdir -p "$dir"
# A stand-in driver: SIGTERM ignored (inherited across exec), its command line a driver's.
standin() { (trap "" TERM; exec -a "node scripts/web/drive.mjs $dir https://127.0.0.1:1/app/" sleep 300) & disown; }

drive held >/dev/null || fail "held reports a driver where none runs"
standin; standin
# Started: both show before the check, by their command line.
for _ in $(seq 1 50); do [ "$(pgrep -cf "scripts/web/drive.mjs $dir " || true)" = 2 ] && break; sleep 0.1; done
[ "$(pgrep -cf "scripts/web/drive.mjs $dir " || true)" = 2 ] || fail "the stand-in drivers did not start"
held=$(drive held) && fail "held does not report two drivers serving the directory"
[ "$(wc -w <<<"$held")" = 2 ] || fail "held names $(wc -w <<<"$held") pids, not 2: $held"
out=$(drive kill)
[ "$(pgrep -cf "scripts/web/drive.mjs $dir " || true)" = 0 ] || fail "kill left a driver serving the directory"
grep -q '2 forced' <<<"$out" || fail "kill did not say it forced the two that ignored SIGTERM: $out"
drive held >/dev/null || fail "held reports a driver after kill"
ok "kill ends every driver for a directory, forcing those that ignore SIGTERM; held says when one serves it"
