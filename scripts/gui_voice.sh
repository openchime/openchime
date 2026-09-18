#!/usr/bin/env bash
# Voice input in the Win32 client, end to end (REQ-296-300, ARCH-112).
#
#   scripts/gui_voice.sh            # run, and leave the client running
#   scripts/gui_voice.sh --kill     # ... and shut it down afterwards
#
# A real daemon recognizing real speech: read-aloud renders a sentence, and the
# client's synthetic microphone (OPENCHIME_TEST_AUDIO=synthetic with
# OPENCHIME_TEST_MIC) speaks it, so every step from capture to the composer or
# the channel is the shipped code. It asserts that
#
#   - push to talk, by the held key, by UI Automation and by the mouse, puts the
#     words in the composer and sends nothing;
#   - free talk posts them as a message and leaves the composer alone;
#   - leaving the conversation, and opening the video recorder, end the session;
#   - a microphone the operating system blocks is reported as that;
#   - with voice input off in the daemon, the controls are not there at all.
#
# Its own daemon on its own port, wiped each run, for the reasons gui_smoke.sh
# gives. Not in CI, as the smoke is not: the daemon cannot run on a Windows runner.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVE="$HERE/scripts/gui_drive.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
WIN_DIR='C:\Windows\Temp\octest'
START=$SECONDS
fails=0
checks=0

export OC_DEV_PORT="${OC_DEV_PORT:-9510}"
export OC_DEV_DIR="${OC_DEV_DIR:-/tmp/oc-voice}"
export OC_DEV_WS="${OC_DEV_WS:-Voice Fixture}"

SENTENCE="Please send the quarterly report to Dana before the meeting on Thursday."
HEARD_RE='quarterly report'          # what must come back, case-insensitively

say()  { printf '%s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); }
ok()   { printf '  ok   %s\n' "$*"; }

# Stop the fixture daemon by its ENVIRONMENT (see gui_smoke.sh for why).
kill_dev_daemon() {
  local p
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
      grep -qx "OPENCHIME_PROTO_PORT=$OC_DEV_PORT" && kill "$p" 2>/dev/null
  done
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    (exec 3<>/dev/tcp/127.0.0.1/"$OC_DEV_PORT") 2>/dev/null || return 0
    exec 3<&- 3>&-; sleep 0.3
  done
  return 1
}

fresh_fixture() {
  kill_dev_daemon || { say "a daemon still holds :$OC_DEV_PORT; stop it and re-run"; exit 1; }
  rm -rf "$OC_DEV_DIR"
}

drive() {
  checks=$((checks + 1))
  if ! "$DRIVE" "$@" >/dev/null 2>&1; then
    fail "the client did not answer '$*'"
    return 1
  fi
  checks=$((checks - 1))
  return 0
}

snap() { "$DRIVE" dump voice >/dev/null 2>&1; cat "$LIN_DIR/voice.txt" 2>/dev/null; }
dict() { snap | grep '^dictate '; }
key_of() { printf '%s' "$1" | grep -o "\b$2=[^ ]*" | head -1 | cut -d= -f2; }
ed_text() { snap | grep '^ed len=' | sed 's/.* text="\(.*\)"$/\1/'; }
msgs_in() { snap | grep -E "^  ch [0-9]+ \"$1\"" | grep -o 'msgs=[0-9]*' | cut -d= -f2; }
prev_in() { snap | grep -E "^  ch [0-9]+ \"$1\"" | sed 's/.* prev="\([^"]*\)".*/\1/'; }

wait_dict() {                      # wait_dict <key> <want> [ms]
  local t=0 ms="${3:-8000}"
  while :; do
    [ "$(key_of "$(dict)" "$1")" = "$2" ] && return 0
    [ "$t" -ge "$ms" ] && return 1
    sleep 0.1; t=$((t + 100))
  done
}

check() {                          # check <label> <command...>
  local label="$1"; shift
  checks=$((checks + 1))
  if "$@"; then ok "$label"; else fail "$label"; fi
}

# Wait for the words in the composer.
heard_in_composer() {
  local t=0
  while [ "$t" -lt 15000 ]; do
    ed_text | grep -qi "$HEARD_RE" && return 0
    sleep 0.2; t=$((t + 200))
  done
  return 1
}

# How long the microphone must stay open for the whole sentence to be spoken.
clip_secs() { python3 -c "import wave; w=wave.open('$LIN_DIR/voice-mic.wav'); print(int(w.getnframes()/w.getframerate())+1)"; }

# --- the sentence the microphone will speak --------------------------------
say "== a sentence to say"
make -C "$HERE" >/dev/null || { say "daemon build failed"; exit 1; }
"$HERE/openchimed" --tts-say expr-voice-2-f "$SENTENCE" /tmp/oc-voice-say.wav 2>/dev/null ||
  { say "read-aloud could not render the sentence"; exit 1; }
mkdir -p "$LIN_DIR"
# 24 kHz from read-aloud to the 16 kHz the microphone runs at, with quiet before
# and after so the detector sees an utterance begin and end.
python3 - /tmp/oc-voice-say.wav "$LIN_DIR/voice-mic.wav" <<'PY'
import sys, wave, struct
src = wave.open(sys.argv[1]); n = src.getnframes(); r = src.getframerate()
d = struct.unpack('<%dh' % n, src.readframes(n))
lp = [d[i] if i in (0, n - 1) else (d[i - 1] + 2 * d[i] + d[i + 1]) // 4 for i in range(n)]
step = r / 16000.0; out = []; t = 0.0
while t < n - 1:
    i = int(t); f = t - i; out.append(int(lp[i] * (1 - f) + lp[i + 1] * f)); t += step
pcm = [0] * 8000 + out + [0] * 24000
o = wave.open(sys.argv[2], 'wb'); o.setnchannels(1); o.setsampwidth(2); o.setframerate(16000)
o.writeframes(struct.pack('<%dh' % len(pcm), *pcm)); o.close()
PY
heard=$("$HERE/openchimed" --stt-hear "$LIN_DIR/voice-mic.wav" 2>/dev/null)
says() { printf '%s' "$heard" | grep -qi "$HEARD_RE"; }
check "the daemon hears the sentence the microphone will say (\"$heard\")" says
HOLD=$(clip_secs)

# --- launch against a daemon with voice input on --------------------------
fresh_fixture
export OPENCHIME_TEST_AUDIO=synthetic
export OPENCHIME_TEST_MIC="$WIN_DIR\\voice-mic.wav"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
t=0; until snap | grep -q 'authed=1 connected=1'; do
  [ "$t" -ge 200 ] && { say "the client never authenticated on :$OC_DEV_PORT"; exit 1; }
  sleep 0.1; t=$((t + 1))
done
drive channel general
say "== the controls"
check "voice input is offered in a conversation" wait_dict offered 1
d=$(dict)
check "the microphone button is drawn" test "$(key_of "$d" mic)" != "0,0,0,0"
check "the free-talk toggle is drawn" test "$(key_of "$d" freetalk)" != "0,0,0,0"

# --- push to talk ------------------------------------------------------------
say "== push to talk, by the held key"
before=$(msgs_in general)
drive type ""
drive key ctrl+shift+space
check "the key opens the microphone in push to talk" wait_dict mode 0
sleep "$HOLD"
drive keyup space
check "letting go closes it" wait_dict on 0
check "the words land in the composer" heard_in_composer
check "and nothing was sent" test "$(msgs_in general)" = "$before"

say "== push to talk, by UI Automation"
drive type ""
uia() { powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$HERE/scripts/uia_invoke.ps1")" -Aid "$1" 2>&1 | grep -q '^uia_invoke: OK'; }
check "invoking composer.mic starts talking" uia composer.mic
check "the session is push to talk" wait_dict mode 0
sleep "$HOLD"
check "invoking it again stops" uia composer.mic
check "the words land in the composer" heard_in_composer

say "== push to talk, by the mouse"
drive type ""
m=$(key_of "$(dict)" mic)
mx=$(( ($(echo "$m" | cut -d, -f1) + $(echo "$m" | cut -d, -f3)) / 2 ))
my=$(( ($(echo "$m" | cut -d, -f2) + $(echo "$m" | cut -d, -f4)) / 2 ))
drive mousedown "$mx" "$my"
check "pressing the microphone opens it in push to talk" wait_dict mode 0
sleep "$HOLD"
drive mouseup "$mx" "$my"
check "releasing it closes it" wait_dict on 0
check "the words land in the composer" heard_in_composer

# --- free talk ----------------------------------------------------------------
say "== free talk"
drive type ""
before=$(msgs_in general)
drive key ctrl+shift+t
check "the shortcut turns free talk on" wait_dict mode 1
posted() {
  local t=0
  while [ "$t" -lt $(( (HOLD + 15) * 10 )) ]; do
    [ "$(msgs_in general)" -gt "$before" ] && prev_in general | grep -qi "$HEARD_RE" && return 0
    sleep 0.1; t=$((t + 1))
  done
  return 1
}
check "what was said is posted to the channel" posted
check "and the composer is left alone" test -z "$(ed_text)"
drive key ctrl+shift+t
check "the shortcut turns it off" wait_dict on 0

# --- what ends a session --------------------------------------------------------
say "== what ends a session"
drive mkchan voice-elsewhere
drive channel general
drive dictate free-on
check "free talk is on in #general" wait_dict mode 1
drive channel voice-elsewhere
check "going to another conversation ends it" wait_dict on 0
drive channel general
drive dictate free-on
drive vm open
check "opening the video recorder ends it" wait_dict on 0
check "and hides the controls under the recorder" wait_dict offered 0
drive vm close

# --- a microphone the operating system blocks ---------------------------------
say "== the microphone blocked"
fresh_fixture
OPENCHIME_TEST_AUDIO=mic-denied "$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
t=0; until snap | grep -q 'authed=1 connected=1'; do
  [ "$t" -ge 200 ] && { say "the client never authenticated on :$OC_DEV_PORT"; exit 1; }
  sleep 0.1; t=$((t + 1))
done
drive channel general
drive key ctrl+shift+space
drive keyup space
check "no session opens" test "$(key_of "$(dict)" on)" = 0
blocked() { snap | grep '^toast\[' | grep -q 'Microphone blocked by Windows privacy settings'; }
check "and the user is told it is blocked, and where to allow it" blocked

# --- off in the daemon ------------------------------------------------------------
say "== voice input off"
fresh_fixture
OPENCHIME_STT=0 "$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
t=0; until snap | grep -q 'authed=1 connected=1'; do
  [ "$t" -ge 200 ] && { say "the client never authenticated on :$OC_DEV_PORT"; exit 1; }
  sleep 0.1; t=$((t + 1))
done
drive channel general
d=$(dict)
check "the daemon offers no voice input" test "$(key_of "$d" avail)" = 0
check "and there is no microphone button" test "$(key_of "$d" mic)" = "0,0,0,0"
drive key ctrl+shift+t
check "and the shortcut does nothing" test "$(key_of "$(dict)" on)" = 0
fresh_fixture                                  # the next run starts with voice input on

[ "${1:-}" = "--kill" ] && "$DRIVE" kill >/dev/null 2>&1
echo
if [ "$fails" -eq 0 ]; then
  say "gui_voice: OK — $checks checks in $((SECONDS - START))s"
else
  say "gui_voice: $fails of $checks checks FAILED"
  exit 1
fi
