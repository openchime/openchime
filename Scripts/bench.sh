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
printf '%-12s %-14s %-16s\n' "idle_conns" "peak_rss_mb" "kb_per_conn"
printf '%s\n' "----------------------------------------"
for N in "${CONNS[@]}"; do
  HOLD=$(( N / 5 + 8 ))                       # allow the auth ramp (~6-7 logins/s)
  ( timeout $((HOLD+15)) ./build/bench_load 127.0.0.1 "$PROTO_PORT" "$N" "$HOLD" 0 "$NUSERS" >/dev/null 2>&1 ) &
  BP=$!; peak=$BASE
  while kill -0 $BP 2>/dev/null; do r=$(rss_kb); [ "$r" -gt "$peak" ] && peak=$r; sleep 0.3; done
  printf '%-12s %-14s %-16s\n' "$N" \
    "$(awk -v k=$peak 'BEGIN{printf "%.1f",k/1024}')" "$(( (peak-BASE)/N ))"
done

echo
echo "Message round-trip latency ($LAT_N concurrent senders, isolated channels):"
./build/bench_load 127.0.0.1 "$PROTO_PORT" "$LAT_N" 6 8 "$NUSERS" | sed -n 's/.*\(rtt_ms.*\)/  \1/p'
