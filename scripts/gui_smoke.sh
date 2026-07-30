#!/usr/bin/env bash
# Machine-checked invariants for the Win32 client's chrome.
#
#   scripts/gui_smoke.sh            # launch, assert, leave the client running
#   scripts/gui_smoke.sh --kill     # ... and shut it down afterwards
#
# WHY THIS EXISTS. Three bugs reached the user in one day (WIN-70, WIN-71's
# regression, WIN-72) and every one of them was a *chrome* bug: a native child
# shown in a view it does not belong to, a composer under a surface you cannot
# type into, a hit-box matching the wrong column. None was visible in a Direct2D
# screenshot at the time, and all of them are one boolean each. Booleans belong in
# a script, not in my eyes.
#
# The assertions come from the three predicates in winmain.c — sidebar_kind()
# (what is in the second column), main_is_conversation() (can you type into the
# middle one) and window_is_covered() (does something own the window) — read back
# through the test hook's `natives` line. A new view or overlay must be added here
# as well as there; that is the point.
#
# NOT IN CI. The daemon is epoll-based, so it is Linux-only, and GitHub's Windows
# runners cannot host it (no Linux containers). A hosted GUI smoke therefore needs
# a self-hosted Windows box with the daemon reachable, which does not exist yet.
# Until it does, this is the pre-push gate for any change to Win32 chrome — run it
# and read it, the same discipline as reading CI.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVE="$HERE/scripts/gui_drive.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
fails=0
checks=0

say()  { printf '%s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); }
ok()   { printf '  ok   %s\n' "$*"; }

# Read one `key=value` out of a fresh dump.
snap() { "$DRIVE" dump smoke >/dev/null 2>&1; cat "$LIN_DIR/smoke.txt" 2>/dev/null; }

# expect <dump> <key> <wanted> <label>
expect() {
  local dump="$1" key="$2" want="$3" label="$4" got
  checks=$((checks + 1))
  got=$(printf '%s' "$dump" | grep -o "\b$key=[^ ]*" | head -1 | cut -d= -f2)
  if [ "$got" = "$want" ]; then ok "$label ($key=$got)"
  else fail "$label — expected $key=$want, got ${got:-<missing>}"; fi
}

say "== launching"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
# Wait for auth rather than sleeping a guess: every assertion below needs a model.
for _ in $(seq 1 40); do
  d=$(snap)
  case "$d" in *"authed=1 connected=1"*) break;; esac
  sleep 0.5
done
case "$(snap)" in
  *"authed=1"*) say "   authed";;
  *) say "   NOT AUTHED — is the dev daemon up? (see gui_drive.sh launch)"; exit 1;;
esac
# Settle before asserting. `authed=1` means the model is live, not that the shell
# has painted — and every `natives` value is measured during paint. A run started
# immediately after a rebuild-and-relaunch failed 11 checks for this reason, then
# passed twice; a test that is right most of the time is not one you can read.
"$DRIVE" view 0 >/dev/null 2>&1
"$DRIVE" channel general >/dev/null 2>&1
sleep 1.5

# --- the view matrix -------------------------------------------------------
# view  sbkind  re  find  ffind  conv     what it is
#  0    1       1   1     0      1        Home: channel list + transcript
#  1    2       0   0     0      0        DMs: conversation list, index in the middle
#  2    3       1   0     0      1        Activity: feed left, transcript stays
#  3    4       0   0     1      0        Files: own column + its own search box
#  4    5       0   0     0      0        Later: own column (WIN-73), Files-shaped
#  5    0       0   0     0      0        Admin: no second column
say "== views"
while read -r v sbkind re find ffind conv name; do
  [ -z "${v:-}" ] && continue
  "$DRIVE" view "$v" >/dev/null 2>&1
  [ "$v" = "0" ] && "$DRIVE" channel general >/dev/null 2>&1
  sleep 0.6
  d=$(snap)
  expect "$d" sbkind "$sbkind" "$name: second column"
  expect "$d" re     "$re"     "$name: composer child"
  expect "$d" find   "$find"   "$name: find box"
  expect "$d" ffind  "$ffind"  "$name: files search box"
  expect "$d" conv   "$conv"   "$name: middle column typeable"
  expect "$d" covered 0        "$name: nothing covering"
done <<'MATRIX'
0 1 1 1 0 1 Home
1 2 0 0 0 0 DMs
2 3 1 0 0 1 Activity
3 4 0 0 1 0 Files
4 5 0 0 0 0 Later
5 0 0 0 0 0 Admin
MATRIX

# --- overlays --------------------------------------------------------------
# Each of these hid a real bug: the search box surviving a modal, the composer
# living under the Pins tab, a native child punched through a dimmed card.
say "== overlays"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; sleep 0.5

"$DRIVE" search "" >/dev/null 2>&1; sleep 0.6
d=$(snap)
expect "$d" srch 1 "search overlay: query box shown"
expect "$d" re   0 "search overlay: composer hidden"
"$DRIVE" search "" >/dev/null 2>&1; sleep 0.4    # toggle off

"$DRIVE" tab 2 >/dev/null 2>&1; sleep 0.6        # Pins
d=$(snap)
expect "$d" re 0 "pins tab: composer hidden"
"$DRIVE" tab 0 >/dev/null 2>&1; sleep 0.4

"$DRIVE" prefs >/dev/null 2>&1; sleep 0.6
d=$(snap)
expect "$d" covered 1 "preferences modal: window covered"
expect "$d" re      0 "preferences modal: composer hidden"
expect "$d" find    0 "preferences modal: find box hidden"
"$DRIVE" keys 0 >/dev/null 2>&1                  # close whatever is open
"$DRIVE" view 0 >/dev/null 2>&1; sleep 0.5

"$DRIVE" palette >/dev/null 2>&1; sleep 0.6
d=$(snap)
expect "$d" pal     1 "command palette: its box shown"
expect "$d" covered 1 "command palette: window covered"
"$DRIVE" palette >/dev/null 2>&1; sleep 0.4

# --- global shortcuts ------------------------------------------------------
# Every one of these was DEAD while the composer had focus, which is nearly
# always: they were handled in the main window's WM_KEYDOWN, and a native child
# consumes what it does not recognise. The shortcut sheet advertised them anyway.
# They are dispatched from the message loop now, so the assertions are made with
# the caret sitting in the message box — the case that was broken.
say "== global shortcuts (composer focused)"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; sleep 0.5
"$DRIVE" type "typing when the shortcut arrives" >/dev/null 2>&1; sleep 0.3

"$DRIVE" key ctrl+k >/dev/null 2>&1; sleep 0.6
expect "$(snap)" pal 1 "Ctrl+K opens the palette from the composer"
"$DRIVE" key esc >/dev/null 2>&1; sleep 0.5
expect "$(snap)" pal 0 "Esc closes it"

"$DRIVE" key ctrl+f >/dev/null 2>&1; sleep 0.6
expect "$(snap)" srch 1 "Ctrl+F opens search from the composer"
"$DRIVE" key esc >/dev/null 2>&1; sleep 0.5
expect "$(snap)" srch 0 "Esc closes search"

"$DRIVE" key ctrl+slash >/dev/null 2>&1; sleep 0.6
expect "$(snap)" modal keys "Ctrl+/ opens the shortcut sheet"
"$DRIVE" key esc >/dev/null 2>&1; sleep 0.5
expect "$(snap)" modal none "Esc closes the sheet"

# --- the modal frame -------------------------------------------------------
# Explicit commit is the whole design (ARCH-94 / docs/CLIENT.md): Save persists,
# Cancel and Esc put the snapshot back. A Cancel that silently behaved like Save
# would look identical on screen, so it is asserted rather than eyeballed. The
# clicks are the frame's own footer, whose geometry the frame owns.
say "== modal frame"

# Click the centre of a preference chip, located from the dump rather than from a
# screenshot. Hardcoded coordinates were wrong twice while writing this: the card
# geometry shifted under the new frame, and a chip's width depends on its label.
# The app already reports where it drew each hit-box, so ask it.
click_pref() {                    # click_pref <row> <val>
  local row="$1" val="$2" r
  r=$(snap | grep -o "prefhit row=$row val=$val r=[0-9,]*" | head -1 | sed 's/.*r=//')
  [ -n "$r" ] || { fail "no prefhit row=$row val=$val in the dump"; return 1; }
  local l t rt b
  IFS=, read -r l t rt b <<<"$r"
  "$DRIVE" click $(( (l + rt) / 2 )) $(( (t + b) / 2 )) >/dev/null 2>&1
}

"$DRIVE" prefs >/dev/null 2>&1; sleep 0.7
d=$(snap)
expect "$d" modal prefs "preferences opens as a modal"
before=$(printf '%s' "$d" | grep -o 'time24=[0-9]' | cut -d= -f2)
other=$([ "$before" = "1" ] && echo 0 || echo 1)

click_pref 1 "$other"; sleep 0.4
expect "$(snap)" time24 "$other" "a change applies while the sheet is open"
"$DRIVE" key esc >/dev/null 2>&1; sleep 0.5
d=$(snap)
expect "$d" closed_by esc       "Esc closes it"
expect "$d" time24    "$before" "Esc RESTORES the snapshot"
expect "$d" modal     none      "nothing left open"

"$DRIVE" prefs >/dev/null 2>&1; sleep 0.7
click_pref 1 "$other"; sleep 0.3
"$DRIVE" key enter >/dev/null 2>&1; sleep 0.6
d=$(snap)
expect "$d" closed_by save     "Enter commits (primary is Save)"
expect "$d" time24    "$other" "the change SURVIVES a commit"

# Leave the preference as it was found: a smoke run that mutates settings is a
# smoke run you stop trusting.
"$DRIVE" prefs >/dev/null 2>&1; sleep 0.6
click_pref 1 "$before"; sleep 0.3
"$DRIVE" key enter >/dev/null 2>&1; sleep 0.5
expect "$(snap)" time24 "$before" "the run left the setting as it found it"

# --- the generic form on the modal frame (WIN-77) ---------------------------
# Sixteen call sites went through a native GDI popup with its own window class and
# its own message loop. Now it is the app's modal frame with native EDITs on it, so
# the things that were previously unassertable are asserted: that Cancel does not
# commit, that Enter does, and that a checkbox and a choice chip actually change
# the value the caller receives.
say "== form"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; sleep 0.5

"$DRIVE" form 3 >/dev/null 2>&1; sleep 0.8
d=$(snap)
expect "$d" modal   form "form opens on the modal frame"
expect "$d" form    1    "the form flag is set"
expect "$d" nfields 3    "all three fields are up"
expect "$d" covered 1    "the window is covered"
expect "$d" re      0    "the composer is hidden under it"
checks=$((checks + 1))
if printf '%s' "$d" | grep -q 'formfield 0 kind=0 edit=1'; then ok "the text field has a native EDIT"
else fail "no native EDIT for field 0"; fi

# Esc must NOT commit. The value is read back from the caller's array via `last`.
"$DRIVE" key esc >/dev/null 2>&1; sleep 0.6
d=$(snap)
expect "$d" modal none "Esc closes the form"
checks=$((checks + 1))
if printf '%s' "$d" | grep -q 'last=cancel'; then ok "Esc means CANCEL"
else fail "Esc — expected last=cancel, got $(printf '%s' "$d" | grep -o 'last=[a-z]*')"; fi

# Enter commits, and a chip click reaches the caller's value.
"$DRIVE" form 3 >/dev/null 2>&1; sleep 0.8
d=$(snap)
# The checkbox row: click the hit-box the app reported rather than a guess.
r=$(printf '%s' "$d" | grep -o 'formfield 1 .*r=[0-9,.-]*' | sed 's/.*r=//')
"$DRIVE" key enter >/dev/null 2>&1; sleep 0.6
d=$(snap)
expect "$d" modal none "Enter closes the form"
checks=$((checks + 1))
if printf '%s' "$d" | grep -q 'last=ok text="initial"'; then ok "Enter COMMITS the field values"
else fail "Enter — expected last=ok with the text intact, got $(printf '%s' "$d" | grep -o 'last=.*')"; fi

# --- pane headers close with a ✕, not a caption (WIN-77) --------------------
say "== pane header"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; sleep 0.5
"$DRIVE" search "" >/dev/null 2>&1; sleep 0.7
expect "$(snap)" srch 1 "search pane is up"
# The ✕'s rect is frame-owned and reported in the dump, so the click comes from
# the app's own geometry rather than from arithmetic that breaks the moment the
# members pane is open and the middle column stops ending at the window edge.
r=$(snap | grep -o 'paneclose=[0-9,.-]*' | head -1 | cut -d= -f2)
if [ -n "${r:-}" ] && [ "${r%%,*}" != "0" ]; then
  IFS=, read -r pl pt pr pb <<<"$r"
  "$DRIVE" click $(( (${pl%.*} + ${pr%.*}) / 2 )) $(( (${pt%.*} + ${pb%.*}) / 2 )) >/dev/null 2>&1
  sleep 0.6
  expect "$(snap)" srch 0 "the pane close button closes it"
else
  checks=$((checks + 1)); fail "no paneclose= rect in the dump"
  "$DRIVE" key esc >/dev/null 2>&1
fi

# --- the composer cue tracks the conversation ------------------------------
# It was a cached global that went stale on a channel switch ("Message bob" while
# reading alice), so it is asserted rather than eyeballed.
say "== composer cue"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; sleep 0.6
checks=$((checks + 1))
cue=$(snap | grep -o 'composer_cue="[^"]*"' | cut -d'"' -f2)
if [ "$cue" = "Message #general" ]; then ok "cue names the channel ($cue)"
else fail "cue — expected \"Message #general\", got \"${cue:-<missing>}\""; fi

[ "${1:-}" = "--kill" ] && "$DRIVE" kill >/dev/null 2>&1

say ""
if [ "$fails" -eq 0 ]; then say "gui_smoke: OK — $checks checks"; exit 0; fi
say "gui_smoke: FAILED — $fails of $checks checks"; exit 1
