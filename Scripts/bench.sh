#!/usr/bin/env bash
# Capacity benchmark (REQ-210/211). Starts a local daemon, drives it with the
# bench_load client, and reports (a) daemon resident memory per idle connection
# and (b) message round-trip latency under concurrency. Everything is torn down
# at the end. Localhost, single box — an upper bound on latency and a lower
# bound on capacity per unit RAM.
#
# Usage: Scripts/bench.sh [idle_conn_counts...]   (default: 50 100 200)
set -uo pipefail
cd "$(dirname "$0")/.."

CONNS=("$@"); [ ${#CONNS[@]} -eq 0 ] && CONNS=(50 100 200)
NUSERS=64          # distinct bootstrap users (auth is 600k-PBKDF2, so keep modest)
LAT_N=32           # concurrent senders for the latency measurement
PROTO_PORT=18443; HEALTH_PORT=18080

echo "Building daemon + bench client..."
make -s all bench

WORK="$(mktemp -d)"
DAEMON_PID=""
cleanup() { [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

USERS=""; for i in $(seq 0 $((NUSERS-1))); do USERS+="bench$i:benchpw:member,"; done; USERS=${USERS%,}

OPENCHIME_DB_PATH="$WORK/bench.db" OPENCHIME_TLS_CERT="$WORK/cert.pem" \
OPENCHIME_TLS_KEY="$WORK/key.pem" OPENCHIME_BLOB_DIR="$WORK/blobs" \
OPENCHIME_PROTO_PORT="$PROTO_PORT" OPENCHIME_HEALTH_PORT="$HEALTH_PORT" \
OPENCHIME_MAX_CONNS_PER_IP=8192 OC_BOOTSTRAP_USERS="$USERS" \
OPENCHIME_MAINT_INTERVAL_MS="${OPENCHIME_MAINT_INTERVAL_MS:-}" \
OPENCHIME_ATTACH_MAX_AGE_DAYS="${OPENCHIME_ATTACH_MAX_AGE_DAYS:-}" \
OPENCHIME_AUDIT_MAX_DAYS="${OPENCHIME_AUDIT_MAX_DAYS:-}" \
  ./openchimed > "$WORK/daemon.log" 2>&1 &
DAEMON_PID=$!

echo "Bootstrapping $NUSERS users (600k-iteration PBKDF2 each) + starting listener..."
for _ in $(seq 1 400); do
  grep -q "netloop: listening" "$WORK/daemon.log" 2>/dev/null && break
  kill -0 "$DAEMON_PID" 2>/dev/null || { echo "daemon exited early:"; cat "$WORK/daemon.log"; exit 1; }
  sleep 0.2
done

rss_kb() { awk '/^VmRSS:/{print $2}' "/proc/$DAEMON_PID/status" 2>/dev/null || echo 0; }
BASE=$(rss_kb)
printf '\nBaseline daemon RSS: %.1f MB\n\n' "$(awk -v k=$BASE 'BEGIN{print k/1024}')"
printf '%s\n' "--------------------------------------------------------"
printf '%-12s %-12s %-14s %-16s\n' "requested" "connected" "peak_rss_mb" "kb_per_conn"
for N in "${CONNS[@]}"; do
  # Auth is a 600k-iteration PBKDF2 serialized on the writer (~2 logins/s here),
  # so the ramp is N/2 seconds, not N/5.
  HOLD=$(( N / 2 + 10 ))
  OUT="$WORK/idle_$N.txt"
  ( timeout $((HOLD+120)) ./build/bench_load 127.0.0.1 "$PROTO_PORT" "$N" "$HOLD" 0 "$NUSERS" >"$OUT" 2>&1 ) &
  BP=$!; peak=$BASE
  while kill -0 $BP 2>/dev/null; do r=$(rss_kb); [ "$r" -gt "$peak" ] && peak=$r; sleep 0.3; done
  # Divide by the connections that ACTUALLY established. Dividing by the
  # requested count understated per-connection memory by >2x whenever the auth
  # ramp outran the client timeout.
  OK=$(sed -n 's/.*connections_ok=\([0-9]*\)\/.*/\1/p' "$OUT"); OK=${OK:-0}
  if [ "$OK" -gt 0 ]; then PER=$(( (peak-BASE)/OK )); else PER="n/a"; fi
  printf '%-12s %-12s %-14s %-16s\n' "$N" "$OK" \
    "$(awk -v k=$peak 'BEGIN{printf "%.1f",k/1024}')" "$PER"
done

echo
echo "Message round-trip latency ($LAT_N concurrent senders, isolated channels):"
# Print the WHOLE line, including connections_ok= and sends=. Stripping the
# prefix hid a degenerate run: failed connections report rtt 0.00, which reads
# like a fantastic result instead of a broken measurement.
# 20s, not 6: auth is a 600k-iteration PBKDF2 on the single writer (~6-7
# logins/s, BENCHMARK.md), so a burst of $LAT_N simultaneous logins queues for
# several seconds. A 6s window let most connections time out before they ever
# authenticated, and the run then reported rtt 0.00 from the handful that made
# it -- a broken measurement that looked like a great one.
./build/bench_load 127.0.0.1 "$PROTO_PORT" "$LAT_N" 20 8 "$NUSERS" | sed 's/^/  /'
