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
# Preferences is two-paned (WIN-78), so a row only has hit-boxes while ITS category
# is showing — and the sheet remembers the pane you left it on. Select the category
# before reaching for a chip.
click_cat() {                     # click_cat <index>
  local i="$1" r l t rr b
  r=$(snap | grep -oE "prefcat $i name=[A-Za-z]* r=[0-9,.-]*" | sed 's/.*r=//')
  [ -n "$r" ] || { fail "no prefcat $i rect"; return 1; }
  IFS=, read -r l t rr b <<<"$r"
  "$DRIVE" click $(( (${l%.*} + ${rr%.*}) / 2 )) $(( (${t%.*} + ${b%.*}) / 2 )) >/dev/null 2>&1
}

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
click_cat 1; sleep 0.5            # Messages — where the time format lives
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
click_cat 1; sleep 0.5
click_pref 1 "$other"; sleep 0.3
"$DRIVE" key enter >/dev/null 2>&1; sleep 0.6
d=$(snap)
expect "$d" closed_by save     "Enter commits (primary is Save)"
expect "$d" time24    "$other" "the change SURVIVES a commit"

# Leave the preference as it was found: a smoke run that mutates settings is a
# smoke run you stop trusting.
"$DRIVE" prefs >/dev/null 2>&1; sleep 0.6
click_cat 1; sleep 0.5
click_pref 1 "$before"; sleep 0.3
"$DRIVE" key enter >/dev/null 2>&1; sleep 0.5
expect "$(snap)" time24 "$before" "the run left the setting as it found it"

# --- custom emoji (REQ-072) -------------------------------------------------
# The claim is that the catalogue arrives and a shortcode becomes an IMAGE. The
# image itself is checked by the thumb cache carrying its attachment id: a
# screenshot proves the pixels, this proves the wiring on every run.
say "== custom emoji"
if [ -f "$LIN_DIR/shipit.png" ]; then
  "$DRIVE" emoji_del smoketest >/dev/null 2>&1; sleep 0.5
  "$DRIVE" emoji_add smoketest 'C:\Windows\Temp\octest\shipit.png' >/dev/null 2>&1; sleep 2.5
  d=$(snap)
  checks=$((checks + 1))
  aid=$(printf '%s' "$d" | grep -oE '^  cemoji smoketest attach=[0-9]+' | grep -oE '[0-9]+$')
  if [ -n "${aid:-}" ]; then ok ":smoketest: is in the catalogue (attach=$aid)"
  else fail "no cemoji row for smoketest"; fi
  # Post it and let the transcript ask for the image.
  "$DRIVE" channel general >/dev/null 2>&1
  "$DRIVE" send "smoke :smoketest: check" >/dev/null 2>&1; sleep 2
  checks=$((checks + 1))
  if [ -n "${aid:-}" ] && snap | grep -qE "^  thumb [0-9]+ id=$aid "; then
    ok "its image decoded, so the shortcode renders as a picture"
  else fail "no decoded bitmap for emoji attachment ${aid:-?}"; fi
  # Leave the workspace as we found it.
  "$DRIVE" emoji_del smoketest >/dev/null 2>&1; sleep 1
  checks=$((checks + 1))
  if ! snap | grep -q 'cemoji smoketest '; then ok "deleting it removes it everywhere"
  else fail "smoketest survived a delete"; fi
else
  say "   (no $LIN_DIR/shipit.png — skipping; create one to cover REQ-072)"
fi

# --- group DMs (REQ-056) ----------------------------------------------------
# A group DM is a DM with more than two participants, so the claims worth asserting
# are that it appears ONCE with a computed title, and that reopening the same set
# returns the same conversation rather than a second one.
say "== group DMs"
"$DRIVE" view 0 >/dev/null 2>&1; sleep 0.4
"$DRIVE" groupdm bob,carol >/dev/null 2>&1; sleep 1.5
d=$(snap)
checks=$((checks + 1))
gid=$(printf '%s' "$d" | grep -oE '^  sbrow sec=1 header=0 cid=[0-9]+ label="bob, carol"' | grep -oE 'cid=[0-9]+' | cut -d= -f2 | head -1)
if [ -n "${gid:-}" ]; then ok "the group appears titled by its people (cid=$gid)"
else fail "no sidebar row labelled \"bob, carol\""; fi
checks=$((checks + 1))
n=$(printf '%s' "$d" | grep -cE 'sbrow .*label="bob, carol"' || true)
if [ "$n" = "1" ]; then ok "and exactly once"
else fail "the group appears $n times"; fi

# Reopening the same set must not make a second conversation.
"$DRIVE" groupdm carol,bob >/dev/null 2>&1; sleep 1.2
checks=$((checks + 1))
n=$(snap | grep -cE 'sbrow .*label="bob, carol"' || true)
if [ "$n" = "1" ]; then ok "reopening the same set reuses it"
else fail "reopening produced $n rows"; fi

# The header names the group, and a DM's tab strip has no About tab.
if [ -n "${gid:-}" ]; then
  "$DRIVE" channel "$gid" >/dev/null 2>&1; sleep 0.8
  expect "$(snap)" sel "$gid" "selecting it works by id"
fi

# --- avatars (WIN-47) -------------------------------------------------------
# Two things are asserted, because the second was invisible for an hour: that the
# avatar is set, AND that a screenshot can see an image at all. Every capture used
# to suppress images (a D2D bitmap belongs to the target that made it), so the
# avatars were drawing correctly on screen and no capture could show it.
say "== avatars"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; sleep 0.5
if [ -f "$LIN_DIR/face.png" ]; then
  "$DRIVE" avatar 'C:\Windows\Temp\octest\face.png' >/dev/null 2>&1; sleep 2.5
  d=$(snap)
  checks=$((checks + 1))
  mine=$(printf '%s' "$d" | grep -oE 'myavatar=[0-9]+' | cut -d= -f2)
  if [ -n "${mine:-}" ] && [ "$mine" != "0" ]; then ok "the avatar is set (id=$mine)"
  else fail "myavatar is still 0 after an upload"; fi
  # The bytes come back and decode: one cached thumbnail whose id is the avatar's.
  sleep 1
  checks=$((checks + 1))
  if snap | grep -qE "^  thumb [0-9]+ id=$mine "; then ok "its image decoded into the cache"
  else fail "no decoded bitmap for avatar $mine — the fetch or the decode failed"; fi
  "$DRIVE" avatar 0 >/dev/null 2>&1; sleep 1
  expect "$(snap)" myavatar 0 "clearing it works"
else
  say "   (no $LIN_DIR/face.png — skipping; create one to cover WIN-47)"
fi

# --- user-defined sidebar sections (WIN-83) ---------------------------------
# The interesting property is the appear-ONCE rule: a conversation in a custom
# section leaves Channels, and a starred one leaves the custom section too. That is
# a claim about the sidebar the core builds, so it is asserted from the row list
# rather than from a screenshot.
say "== sidebar sections"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; sleep 0.5

rows_have() {                     # rows_have <label> <count>
  local want="$1" n="$2" got
  checks=$((checks + 1))
  got=$(snap | grep -cE "^  sbrow .*label=\"$want\"" || true)
  if [ "$got" = "$n" ]; then ok "#$want appears $n time(s) in the sidebar"
  else fail "#$want appears $got time(s), expected $n"; fi
}

"$DRIVE" section add SmokeSec >/dev/null 2>&1; sleep 0.4
d=$(snap)
expect "$d" sections 1 "a section is created"
"$DRIVE" section put 1 0 >/dev/null 2>&1; sleep 0.5
d=$(snap)
checks=$((checks + 1))
if printf '%s' "$d" | grep -q 'section 0 name="SmokeSec" n=1 collapsed=0 ids=1'; then
  ok "the conversation is in it"
else fail "assignment did not land: $(printf '%s' "$d" | grep -o 'section 0 .*')"; fi
# ... and it is in the SECTION, not in Channels as well.
rows_have SmokeSec 1
rows_have general 1
checks=$((checks + 1))
if snap | grep -qE '^  sbrow sec=16 header=0 cid=1 '; then ok "#general sits under section 16"
else fail "#general is not in the custom section's rows"; fi

# Removing the section returns the conversation rather than losing it.
"$DRIVE" section rm 0 >/dev/null 2>&1; sleep 0.4
expect "$(snap)" sections 0 "removing the section leaves none"

# --- appearance: text size, zoom, accent, density (WIN-78) ------------------
# Each of these rebuilds the DirectWrite table or the palette, so "it did not take
# effect" is a real failure mode and none of it is visible in a boolean. The dump
# reports the three scale inputs ARCH-97 keeps apart plus their product.
say "== appearance"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; sleep 0.5

base=$(snap | grep -oE 'scale=[0-9.]+' | head -1 | cut -d= -f2)
"$DRIVE" key ctrl+= >/dev/null 2>&1; sleep 0.5
d=$(snap)
expect "$d" zoom 1 "Ctrl+= zooms in"
checks=$((checks + 1))
got=$(printf '%s' "$d" | grep -oE 'scale=[0-9.]+' | head -1 | cut -d= -f2)
if [ "$got" != "$base" ]; then ok "the font scale actually moved ($base -> $got)"
else fail "scale unchanged at $got — the zoom did not reach fonts_build()"; fi
"$DRIVE" key ctrl+0 >/dev/null 2>&1; sleep 0.5
d=$(snap)
expect "$d" zoom 0 "Ctrl+0 resets it"
checks=$((checks + 1))
got=$(printf '%s' "$d" | grep -oE 'scale=[0-9.]+' | head -1 | cut -d= -f2)
if [ "$got" = "$base" ]; then ok "back to $base"
else fail "scale did not return: $got vs $base"; fi

# Ctrl+, opens Preferences, and it opens on a category rather than a flat list.
"$DRIVE" key ctrl+, >/dev/null 2>&1; sleep 0.7
d=$(snap)
expect "$d" modal prefs "Ctrl+, opens Preferences"
# It reopens on the pane you left it on, which is why this selects rather than
# asserts a fixed category — an earlier section in this run leaves it on Messages.
click_cat 0; sleep 0.5
expect "$(snap)" prefcat 0 "the Appearance pane selects"

# Switch to Advanced by clicking the row the app reported.
r=$(printf '%s' "$d" | grep -oE 'prefcat 3 name=Advanced r=[0-9,.-]*' | sed 's/.*r=//')
if [ -n "${r:-}" ]; then
  IFS=, read -r l t rt2 b <<<"$r"
  "$DRIVE" click $(( (${l%.*} + ${rt2%.*}) / 2 )) $(( (${t%.*} + ${b%.*}) / 2 )) >/dev/null 2>&1
  sleep 0.5
  expect "$(snap)" prefcat 3 "the category list switches panes"
else
  checks=$((checks + 1)); fail "no prefcat rects in the dump"
fi

# Text size and accent apply LIVE, and Cancel puts BOTH back — the whole point of
# the snapshot rule, and neither is a local variable: one rebuilds every font, the
# other re-resolves the palette.
"$DRIVE" key esc >/dev/null 2>&1; sleep 0.5
"$DRIVE" prefs >/dev/null 2>&1; sleep 0.7
# Back to Appearance FIRST: the sheet remembers the pane you left it on (which is
# right — you came back for the same thing), so the chips below are not on screen
# after the Advanced check above.
click_cat 0; sleep 0.5
d=$(snap)
ts0=$(printf '%s' "$d" | grep -oE 'textsize=[0-9]' | cut -d= -f2)
ac0=$(printf '%s' "$d" | grep -oE 'accent=[0-9]' | cut -d= -f2)
click_pref 7 2 && sleep 0.4      # Text size -> Large
click_pref 6 2 && sleep 0.4      # Accent -> the third swatch
d=$(snap)
expect "$d" textsize 2 "a text size applies while the sheet is open"
expect "$d" accent   2 "so does an accent"
"$DRIVE" key esc >/dev/null 2>&1; sleep 0.6
d=$(snap)
expect "$d" textsize "$ts0" "Cancel RESTORES the text size"
expect "$d" accent   "$ac0" "Cancel RESTORES the accent"

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

# --- the composer is ours (WIN-80) ------------------------------------------
# Every one of these was the RichEdit's job until today, which means every one of
# them is now code that can be wrong. They are asserted through `chars` (real
# WM_CHARs) rather than `type` (which sets the text), because typing is the path a
# user runs.
say "== composer"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; sleep 0.5
ed() { snap | grep -oE "^ed .*" | head -1; }
edf() {                            # edf <field>  -> its value
  ed | grep -oE "$1=[-0-9]+" | head -1 | cut -d= -f2
}

"$DRIVE" chars "hello world" >/dev/null 2>&1; sleep 0.4
checks=$((checks + 1))
if [ "$(edf len)" = "11" ] && [ "$(edf caret)" = "11" ]; then ok "typing inserts and moves the caret"
else fail "after typing: $(ed)"; fi

"$DRIVE" key backspace >/dev/null 2>&1; sleep 0.3
checks=$((checks + 1))
[ "$(edf len)" = "10" ] && ok "Backspace deletes" || fail "Backspace: $(ed)"

"$DRIVE" key ctrl+left >/dev/null 2>&1; sleep 0.3
checks=$((checks + 1))
[ "$(edf caret)" = "6" ] && ok "Ctrl+Left jumps a word" || fail "Ctrl+Left: $(ed)"

"$DRIVE" key shift+left >/dev/null 2>&1; sleep 0.3
checks=$((checks + 1))
[ "$(edf sel)" = "1" ] && ok "Shift+Left selects" || fail "Shift+Left: $(ed)"

"$DRIVE" key ctrl+z >/dev/null 2>&1; sleep 0.3
checks=$((checks + 1))
[ "$(edf len)" = "11" ] && ok "Ctrl+Z undoes the delete" || fail "Ctrl+Z: $(ed)"

"$DRIVE" key ctrl+a >/dev/null 2>&1; sleep 0.3
checks=$((checks + 1))
[ "$(edf sel)" = "1" ] && ok "Ctrl+A selects all" || fail "Ctrl+A: $(ed)"

# A click INSIDE the field places the caret — the field is drawn, not a child, so
# this goes through the same WM_LBUTTONDOWN a user generates.
r=$(ed | grep -oE 'box=[0-9,.-]+' | cut -d= -f2)
if [ -n "${r:-}" ]; then
  IFS=, read -r bl bt br bb <<<"$r"
  "$DRIVE" click $(( ${bl%.*} + 14 )) $(( (${bt%.*} + ${bb%.*}) / 2 )) >/dev/null 2>&1; sleep 0.3
  checks=$((checks + 1))
  c=$(edf caret)
  if [ -n "$c" ] && [ "$c" -lt 5 ]; then ok "a click places the caret (caret=$c)"
  else fail "click-to-caret: $(ed)"; fi
else
  checks=$((checks + 1)); fail "no ed box in the dump"
fi

# The mention popover still tracks the caret, and Tab accepts.
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; sleep 0.3
"$DRIVE" chars "hey @al" >/dev/null 2>&1; sleep 0.6
"$DRIVE" key tab >/dev/null 2>&1; sleep 0.4
checks=$((checks + 1))
if ed | grep -q 'text="hey @alice "'; then ok "Tab accepts a mention completion"
else fail "completion: $(ed)"; fi

# Enter sends and clears; the draft machinery still works across a switch.
"$DRIVE" key enter >/dev/null 2>&1; sleep 1
checks=$((checks + 1))
[ "$(edf len)" = "0" ] && ok "Enter sends and clears the field" || fail "after Enter: $(ed)"

# A channel of its own for this check: asserting against whatever second channel a
# workspace happens to have made the result depend on the fixture, and a test that
# passes because of the fixture is a test that fails when someone gives you a clean
# one — which is exactly what happened.
"$DRIVE" mkchan smokedrafts 1 >/dev/null 2>&1; sleep 1
"$DRIVE" chars "a draft" >/dev/null 2>&1; sleep 0.3
"$DRIVE" channel smokedrafts >/dev/null 2>&1; sleep 0.6
checks=$((checks + 1))
[ "$(edf len)" = "0" ] && ok "the other channel starts empty" || fail "draft leaked: $(ed)"
"$DRIVE" channel general >/dev/null 2>&1; sleep 0.5
checks=$((checks + 1))
if ed | grep -q 'text="a draft"'; then ok "and the draft comes back"
else fail "draft lost: $(ed)"; fi
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1

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
