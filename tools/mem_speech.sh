#!/usr/bin/env bash
# The speech models' memory on a deployed machine (TESTING.md, "Speech models'
# memory"). Copies a static demo client and three recorded clips to a Fly
# machine, signs a member in, and for each case samples the daemon's VmRSS and
# the machine's MemAvailable every 0.2 s: idle; a short message read aloud;
# three full pieces read aloud; 5, 20 and 30 s heard. Prints each case's peak,
# its lowest free memory and its time, then removes what it copied.
#
#   tools/mem_speech.sh <fly-app> <user> <password> [channel_id]
#
# The member must be in the conversation (1, general, by default). Needs
# flyctl signed in, and ffmpeg here to turn the clips into the 16 kHz audio the
# daemon hears. Nothing is left on the machine.
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -ge 3 ] || { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
APP="$1" USER_NAME="$2" USER_PW="$3" CH="${4:-1}"
command -v fly >/dev/null || { echo "mem_speech: flyctl is not installed" >&2; exit 1; }
command -v ffmpeg >/dev/null || { echo "mem_speech: ffmpeg is not installed" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# The client, static, so it runs on whatever the machine's image carries; the
# same sources and flags as `make build/demo_client`.
cmd="$(make -n build/demo_client | grep -v '^mkdir' | tr -d '\\\n')"
eval "${cmd/-o build\/demo_client/-static -o $WORK/demo_client}" 2>/dev/null ||
  { echo "mem_speech: the static demo client did not build" >&2; exit 1; }
for s in 5 20 30; do
  ffmpeg -v error -y -i "tools/speech/clip-${s}s.ogg" -f s16le -ac 1 -ar 16000 "$WORK/clip-${s}s.pcm"
done
cp tools/mem_speech_remote.sh "$WORK/run.sh"

R=/tmp/memspeech
fly ssh console -a "$APP" -C "mkdir -p $R" >/dev/null
for f in demo_client run.sh clip-5s.pcm clip-20s.pcm clip-30s.pcm; do
  printf 'put %s %s/%s\n' "$WORK/$f" "$R" "$f" | fly ssh sftp shell -a "$APP" >/dev/null
done
status=0
fly ssh console -a "$APP" -C "sh $R/run.sh $R '$USER_NAME' '$USER_PW' $CH" || status=$?
fly ssh console -a "$APP" -C "rm -rf $R" >/dev/null || true
exit $status
