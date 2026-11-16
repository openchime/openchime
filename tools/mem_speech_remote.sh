#!/bin/sh
# The measuring half of tools/mem_speech.sh, run on the machine with nothing
# but sh: the daemon's VmRSS and the machine's MemAvailable, every 0.2 s, while
# each case runs; one line per case.
#
#   sh mem_speech_remote.sh <dir> <user> <password> <channel_id>
#
# <dir> holds demo_client and clip-{5,20,30}s.pcm. The daemon is found by name
# and its port read from its own environment.
D="$1" U="$2" P="$3" CH="$4"
PID="$(pidof openchimed 2>/dev/null | cut -d' ' -f1)"
[ -n "$PID" ] || { echo "mem_speech: no openchimed on this machine" >&2; exit 1; }
PORT="$(tr '\0' '\n' < /proc/"$PID"/environ | sed -n 's/^OPENCHIME_PORT=//p')"
PORT="${PORT:-443}"

kb() { while read -r k v _; do [ "$k" = "$2" ] && { echo "$v"; return; }; done < "$1"; echo 0; }

# Sample until the file $D/stop appears; leave "peak_rss_kb min_avail_kb".
sample() {
  peak=0 low=999999999
  while [ ! -e "$D/stop" ]; do
    r="$(kb /proc/"$PID"/status VmRSS:)"; a="$(kb /proc/meminfo MemAvailable:)"
    [ "$r" -gt "$peak" ] && peak="$r"; [ "$a" -lt "$low" ] && low="$a"
    sleep 0.2
  done
  echo "$peak $low" > "$D/sampled"
}

# run_case <name> <command...>: the case, sampled, timed by the clock here.
run_case() {
  name="$1"; shift
  rm -f "$D/stop" "$D/sampled"
  sample & sp=$!
  t0="$(date +%s)"
  "$@" > "$D/out" 2>&1; rc=$?
  t1="$(date +%s)"
  touch "$D/stop"; wait "$sp"
  read -r peak low < "$D/sampled"
  secs=$((t1 - t0))
  printf '%-34s peak %4d MB  free at worst %4d MB  time %s s%s\n' "$name" $((peak / 1024)) $((low / 1024)) \
         "$secs" "$( [ $rc = 0 ] || echo "  FAILED: $(tail -1 "$D/out")" )"
}

C="$D/demo_client 127.0.0.1 $PORT $U $P"
PIECE="The quarterly review starts at nine. Please bring the latest figures for each region, and last week's notes."
run_case "idle (5 s)" sleep 5
run_case "a short message read aloud" $C speak "$CH" "A short message, read aloud."
run_case "three full pieces read aloud" $C speak "$CH" "$PIECE $PIECE $PIECE"
run_case "a 5 s segment heard" $C listen "$CH" "$D/clip-5s.pcm"
run_case "a 20 s segment heard" $C listen "$CH" "$D/clip-20s.pcm"
run_case "a 30 s segment heard" $C listen "$CH" "$D/clip-30s.pcm"
