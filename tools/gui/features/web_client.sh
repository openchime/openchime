#!/usr/bin/env bash
# What only the browser client has to show (WEB.md): a video message recorded
# in wasm, the text layer on the canvas backend, About naming the web build,
# and a composition from the input method.
#
#   tools/gui/features/web_client.sh          # run; the client and daemon stop at the end
#   tools/gui/features/web_client.sh --keep   # ... and leave them running
#
# The web client, signed in on the daemon's page, with Chromium's fake
# microphone and the synthetic camera (OPENCHIME_DEV_CAPTURE=synthetic). It
# asserts that
#
#   - the text layer passes its Unicode checks (the `sttest` verb: right-to-left
#     order, clusters hit whole, line breaks; sdltext/st_canvas.c);
#   - a video message: the card opens on the camera, records past three
#     seconds by the recorder's own clock, and Stop takes the file -- the
#     desktop's recorder, codecs and muxer, in wasm;
#   - the audio runs on the Web Audio worklet, not the fallback (audio_web.c);
#   - a composition played into the page's input-method input (plat_web.c)
#     shows composing in the composer, then commits two characters, as a
#     desktop input method's does;
#   - About OpenChime names both builds on one line, the client's as the web's.
#
# The client signing in and drawing itself is smoke.sh's; the screen recorder
# is screenrec.sh's. Its own daemon on its own port, wiped each run. Web only,
# whatever OC_TEST_PLATFORM says.
#
# platforms: web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export OC_TEST_PLATFORM=web
. "$HERE/tools/gui/harness/lib.sh"
gui_begin web_client 9585 /tmp/oc-web-client "Web Fixture" "$@"
export OPENCHIME_DEV_CAPTURE=synthetic   # the synthetic camera: frame numbers in its pixels

# ack_text <verb...>: the whole of the client's answer to the verb (ack_of
# reads its first word only); empty when it did not answer.
ack_text() { "$DRIVE" "$@" 2>/dev/null | sed -n 's/^ack: //p' | head -1; }
has() { grep -E "$1" "$D" >/dev/null; }          # has <regex>, in the last dump
# vm_is <regex...>: a fresh dump whose vm= line matches each.
vm_is() {
  local l r
  gui_snap || return 1
  l="$(grep -m1 '^vm=' "$D")" || return 1
  for r in "$@"; do grep -E "$r" <<<"$l" >/dev/null || return 1; done
}
DRIVELOG="$OC_TEST_DIR/web-a/drive.log"         # the page's console, as the driver logs it

step "the client, signed in, in #general"
gui_launch
act channel general
in_general() { gui_snap && has '^authed=1 .*sel=[1-9]' && has '^ed '; }
waitfor 10 in_general || gui_abort "#general did not open with its composer: $(grep -m1 '^authed=' "$D")"

step "the text layer"
ST="$(ack_text sttest)"
ok_ack() { grep '^ok ' <<<"$1" >/dev/null; }
check "the text layer's checks pass (sttest: ${ST:-no answer})" ok_ack "$ST"

step "a video message, recorded from the camera"
act vm open
check "the video card opens on the camera" waitfor 15 vm_is '^vm=1 ' ' recphase=0 ' ' frame=[1-9]'
act vm record
check "recording began" waitfor 15 vm_is '^vm=1 ' ' recphase=2 '
# Past three seconds, by the recorder's own clock: four digits or more, not 0000-2999.
check "...and ran three seconds" waitfor 20 vm_is '^vm=1 ' ' recphase=2 ' ' rec_elapsed=([3-9][0-9]{3}|[0-9]{5,}) '
act vm stop
check "stopped, and the file taken (take_bytes>0)" waitfor 20 vm_is '^vm=1 ' ' take_bytes=[1-9]'
say "  $(grep -m1 '^vm=' "$D" | cut -c1-200)"
AUDIO=""
audio_said() { AUDIO="$(grep -m1 -oE 'oc-web: audio (worklet|scriptprocessor[^"]*)' "$DRIVELOG" 2>/dev/null)"; [ -n "$AUDIO" ]; }
waitfor 5 audio_said
check "the audio runs on the worklet (${AUDIO:-not said})" [ "$AUDIO" = "oc-web: audio worklet" ]
act vm close

step "a composition from the input method"
act channel general
ed() { grep -m1 '^ed ' "$D"; }
composing() { gui_snap && grep ' comp=2 ' <<<"$(ed)" >/dev/null; }
act ime update にほ
check "composing: two units (comp=2)" waitfor 10 composing
say "  $(ed | cut -c1-200)"
committed() { gui_snap && grep '^ed len=2 ' <<<"$(ed)" >/dev/null && grep '日本' <<<"$(ed)" >/dev/null; }
act ime commit 日本
check "committed: two characters in the composer (len=2, 日本)" waitfor 10 committed
say "  $(ed | cut -c1-200)"

step "About OpenChime"
ABOUT="$(ack_text about)"
both_builds() { grep -E '^OpenChime client [^ ]+ \(web\), server [0-9a-z]+, protocol [0-9]+$' <<<"$1" >/dev/null; }
check "both builds on one line, the client's the web's (${ABOUT:-no answer})" both_builds "$ABOUT"
act key esc

gui_end
