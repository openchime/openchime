#!/usr/bin/env bash
# Voice input in the Win32 client, end to end (REQ-296-300, ARCH-112).
#
#   tools/gui/features/voice.sh            # run; the client and daemon stop at the end
#   tools/gui/features/voice.sh --keep     # ... and leave them running
#
# A real daemon recognizing real speech: read-aloud renders a sentence, and the
# client's synthetic microphone (OPENCHIME_DEV_AUDIO=synthetic with
# OPENCHIME_DEV_MIC) speaks it, so every step from capture to the composer or
# the channel is the shipped code. It asserts that
#
#   - push to talk, by the held key, by UI Automation and by the mouse, puts the
#     words in the composer and sends nothing;
#   - free talk posts them as a message and leaves the composer alone;
#   - leaving the conversation, and opening the video recorder, end the session;
#   - a microphone the operating system blocks is reported as that;
#   - with voice input off in the daemon, the controls are not there at all.
#
# It needs a daemon built with the speech data (read-aloud and recognition),
# and stops, saying so, when the one here has none. Its own daemon on its own
# port, wiped each run, for the reasons tools/gui/features/smoke.sh gives. Not in CI, as
# the smoke is not: the daemon cannot run on a Windows runner.
#
# platforms: win32 web
# needs: speech
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin voice 9680 /tmp/oc-voice "Voice Fixture" "$@"
# Files the client is given (a microphone's clip): in $LIN_DIR (lib.sh), the
# Windows share, named by its Windows path to the Windows client and carried
# onto the page by the driver for the web client.

SENTENCE="Please send the quarterly report to Dana before the meeting on Thursday."
HEARD_RE='quarterly report'          # what must come back, case-insensitively

dk() { field "$D" '^dictate ' "$1"; }                 # dk <key>: the dictate line's
dict_is() { gui_snap && [ "$(dk "$1")" = "$2" ]; }     # dict_is <key> <want>
ed_text() { grep -m1 '^ed len=' "$D" | sed 's/.* text="\(.*\)"$/\1/'; }
msgs_in() { grep -m1 -E "^  ch [0-9]+ [^\"]*\"$1\"" "$D" | grep -o 'msgs=[0-9]*' | cut -d= -f2; }
prev_in() { grep -m1 -E "^  ch [0-9]+ [^\"]*\"$1\"" "$D" | sed 's/.* prev="\([^"]*\)".*/\1/'; }
# The words in the composer.
heard() { gui_snap && grep -i "$HEARD_RE" <<<"$(ed_text)" >/dev/null; }
# A fresh fixture for the next launch: the client and the daemon stopped -- one
# with the speech models loaded takes some seconds to go -- and its directory
# wiped. Each launch is its own (voice on, the microphone blocked, voice off in
# the daemon), so this script launches by itself.
port_free() { ! (exec 3<>/dev/tcp/127.0.0.1/"$OC_TEST_PORT") 2>/dev/null; }
fresh_fixture() {
  "$DRIVE" kill >/dev/null 2>&1 || true
  kill_dev_daemon || waitfor 15 port_free || gui_abort "a daemon still holds :$OC_TEST_PORT"
  rm -rf "$OC_TEST_DIR"; mkdir -p "$OC_TEST_DIR"
}

step "a sentence to say"
if [ "${OC_TEST_NO_BUILD:-0}" != 1 ]; then
  make -C "$HERE" daemon >/dev/null || gui_abort "the daemon, with its speech data, did not build"
fi
SAY="$OC_TEST_DIR/say.wav"
if ! err="$("$HERE/openchimed" --tts-say expr-voice-2-f "$SENTENCE" "$SAY" 2>&1 >/dev/null)"; then
  case "$err" in
    *"unknown argument"*)
      gui_abort "this daemon has no speech data (openchimed has no --tts-say: it was built without the read-aloud and recognition models), so voice input cannot be tested here" ;;
    *) gui_abort "read-aloud could not render the sentence: $(tail -1 <<<"$err")" ;;
  esac
fi
mkdir -p "$LIN_DIR"
# 24 kHz from read-aloud to the 16 kHz the microphone runs at, with quiet before
# and after so the detector sees an utterance begin and end.
python3 -I -c '
import sys, wave, struct
src = wave.open(sys.argv[1]); n = src.getnframes(); r = src.getframerate()
d = struct.unpack("<%dh" % n, src.readframes(n))
lp = [d[i] if i in (0, n - 1) else (d[i - 1] + 2 * d[i] + d[i + 1]) // 4 for i in range(n)]
step = r / 16000.0; out = []; t = 0.0
while t < n - 1:
    i = int(t); f = t - i; out.append(int(lp[i] * (1 - f) + lp[i + 1] * f)); t += step
pcm = [0] * 8000 + out + [0] * 24000
o = wave.open(sys.argv[2], "wb"); o.setnchannels(1); o.setsampwidth(2); o.setframerate(16000)
o.writeframes(struct.pack("<%dh" % len(pcm), *pcm)); o.close()
' "$SAY" "$LIN_DIR/voice-mic.wav" || gui_abort "the sentence could not be made into the microphone's clip"
said="$("$HERE/openchimed" --stt-hear "$LIN_DIR/voice-mic.wav" 2>/dev/null)"
says() { grep -i "$HEARD_RE" <<<"$said" >/dev/null; }
check "the daemon hears the sentence the microphone will say (\"$said\")" says
# How long the microphone must stay open for the whole sentence to be spoken.
HOLD=$(python3 -I -c 'import sys, wave; w = wave.open(sys.argv[1]); print(int(w.getnframes() / w.getframerate()) + 1)' "$LIN_DIR/voice-mic.wav")

step "the controls"
fresh_fixture
export OPENCHIME_DEV_AUDIO=synthetic
export OPENCHIME_DEV_MIC="$(cpath voice-mic.wav)"
gui_launch
act channel general
check "voice input is offered in a conversation" waitfor 8 dict_is offered 1
check "the microphone button is drawn" test "$(dk mic)" != "0,0,0,0"
check "the free-talk toggle is drawn" test "$(dk freetalk)" != "0,0,0,0"

step "push to talk, by the held key"
before=$(msgs_in general)
act type ""
act key ctrl+shift+space
check "the key opens the microphone in push to talk" waitfor 8 dict_is mode 0
# paced: the key is held while the microphone speaks the whole clip, as long as a person takes to say it
sleep "$HOLD"
act keyup space
check "letting go closes it" waitfor 8 dict_is on 0
check "the words land in the composer" waitfor 15 heard
check "and nothing was sent" test "$(msgs_in general)" = "$before"

if [ "$PLAT" = win32 ]; then
step "push to talk, by UI Automation"
act type ""
check "invoking composer.mic starts talking" win_uia_invoke composer.mic
check "the session is push to talk" waitfor 8 dict_is mode 0
# paced: the microphone stays open while it speaks the whole clip
sleep "$HOLD"
check "invoking it again stops" win_uia_invoke composer.mic
check "the words land in the composer" waitfor 15 heard
fi

step "push to talk, by the mouse"
act type ""
gui_snap || gui_abort "the client did not dump"
m=$(dk mic)
mx=$(( ($(echo "$m" | cut -d, -f1) + $(echo "$m" | cut -d, -f3)) / 2 ))
my=$(( ($(echo "$m" | cut -d, -f2) + $(echo "$m" | cut -d, -f4)) / 2 ))
act mousedown "$mx" "$my"
check "pressing the microphone opens it in push to talk" waitfor 8 dict_is mode 0
# paced: the button is held while the microphone speaks the whole clip
sleep "$HOLD"
act mouseup "$mx" "$my"
check "releasing it closes it" waitfor 8 dict_is on 0
check "the words land in the composer" waitfor 15 heard

step "free talk"
act type ""
gui_snap || gui_abort "the client did not dump"
before=$(msgs_in general)
act key ctrl+shift+t
check "the shortcut turns free talk on" waitfor 8 dict_is mode 1
posted() { gui_snap && [ "$(msgs_in general)" -gt "$before" ] && grep -i "$HEARD_RE" <<<"$(prev_in general)" >/dev/null; }
check "what was said is posted to the channel" waitfor $((HOLD + 15)) posted
check "and the composer is left alone" test -z "$(ed_text)"
act key ctrl+shift+t
check "the shortcut turns it off" waitfor 8 dict_is on 0

step "what ends a session"
act mkchan voice-elsewhere
made() { gui_snap && grep -E '^  ch [0-9]+ [^"]*"voice-elsewhere"' "$D" >/dev/null; }
waitfor 10 made || gui_abort "the channel voice-elsewhere did not come back from the daemon"
act channel general
act dictate free-on
check "free talk is on in #general" waitfor 8 dict_is mode 1
act channel voice-elsewhere
check "going to another conversation ends it" waitfor 8 dict_is on 0
act channel general
act dictate free-on
act vm open
check "opening the video recorder ends it" waitfor 8 dict_is on 0
check "and hides the controls under the recorder" waitfor 8 dict_is offered 0
act vm close

step "the microphone blocked"
fresh_fixture
OPENCHIME_DEV_AUDIO=mic-denied gui_launch
act channel general
act key ctrl+shift+space
act keyup space
blocked() { gui_snap && grep -E '^toast\[[0-9]+\].*Microphone blocked' "$D" >/dev/null; }
check "the user is told it is blocked, and where to allow it" waitfor 5 blocked
check "and no session opens" test "$(dk on)" = 0

step "voice input off"
fresh_fixture
OPENCHIME_STT=0 gui_launch
act channel general
gui_snap || gui_abort "the client did not dump"
check "the daemon offers no voice input" test "$(dk avail)" = 0
check "and there is no microphone button" test "$(dk mic)" = "0,0,0,0"
act key ctrl+shift+t
gui_snap || gui_abort "the client did not dump"
check "and the shortcut does nothing" test "$(dk on)" = 0
gui_end
