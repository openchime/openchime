#!/usr/bin/env bash
# Run the GUI feature scripts (tools/gui/features/*.sh), each on every platform
# it declares, and print one table of what they found (tools/gui/README.md,
# "Running").
#
#   tools/gui/run.sh                     # every test, on every platform it declares: Windows, then the web
#   tools/gui/run.sh smoke calls         # those tests, on every platform each declares
#   tools/gui/run.sh --web smoke calls   # ... in the browser only (--win32: on Windows only)
#   tools/gui/run.sh --list              # each test, its platforms and what it needs
#   tools/gui/run.sh --keep smoke        # passed to the scripts: the last one's clients and daemon stay up
#   tools/gui/run.sh --no-build ...      # test the binaries already built
#
# A test declares its platforms in its header (`# platforms: win32 web`) and
# what it needs beyond them (`# needs: speech`); nothing here lists the tests.
# In order, a run:
#   - holds the scripts to their shape (tools/gui/lint.sh), and goes no further
#     if one is out of it;
#   - takes the machine's GUI lock (harness/lock.sh), held to the end;
#   - builds once: the daemon with the web client in it, the Windows client,
#     and build/signin and build/demo_client for a web run,
#     with the read-aloud, voice input and summaries switches as remembered
#     (build/.features), which it leaves as it was; the scripts then build
#     nothing (OC_TEST_NO_BUILD=1), with --no-build or without;
#   - checks what each platform it will run needs, names what is missing, and
#     goes no further if anything is;
#   - runs the web driver check (harness/web/driver_check.sh) before the first
#     web test;
#   - runs each test on each platform, one at a time, with what a script left
#     running taken down before the next. A test whose needs this machine does
#     not meet is skipped, saying why.
# Each script's whole output goes to /tmp/oc-gui/<time>/<test>-<platform>.log,
# and one line per test as it ends here. The table at the end has a row per
# test and a column per platform: "N/M" (passed), "FAILED N/M", "STOPPED at
# <step>", "skipped (<why>)", or "-" for a platform the test does not declare.
# Exits 1 when anything failed or stopped (skipped is not passed, and not a
# failure), 2 when the run could not start.
#
# By hand, never in a suite or CI (docs/TESTING.md): the daemon is Linux's and
# the Windows client needs a desktop.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GUI="$HERE/tools/gui"
usage() { sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; }

want_win32=0; want_web=0; list=0; keep=0; build=1; names=()
for a in "$@"; do
  case "$a" in
    --win32) want_win32=1 ;;
    --web) want_web=1 ;;
    --list) list=1 ;;
    --keep) keep=1 ;;
    --no-build) build=0 ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "run.sh: unknown option $a" >&2; usage >&2; exit 2 ;;
    *) a="${a%.sh}"; names+=("${a#gui_}") ;;
  esac
done
[ "$want_win32$want_web" = 00 ] && { want_win32=1; want_web=1; }

# --- The tests, from their own headers ------------------------------------------
# platforms_of <script> / needs_of <script>: the words of its header line.
platforms_of() { sed -n 's/^# platforms:[[:space:]]*//p' "$1" | head -1; }
needs_of() { sed -n 's/^# needs:[[:space:]]*//p' "$1" | head -1; }
all=()
for f in "$GUI"/features/*.sh; do all+=("$(basename "$f" .sh)"); done

if [ "$list" = 1 ]; then
  printf '%-14s %-12s %s\n' test platforms needs
  for n in "${all[@]}"; do
    f="$GUI/features/$n.sh"
    printf '%-14s %-12s %s\n' "$n" "$(platforms_of "$f")" "$(needs_of "$f")"
  done
  exit 0
fi

[ "${#names[@]}" -gt 0 ] || names=("${all[@]}")
for n in "${names[@]}"; do
  [ -f "$GUI/features/$n.sh" ] || { echo "run.sh: no test named $n (tools/gui/features/$n.sh; --list lists them)" >&2; exit 2; }
done

# The platforms this run uses: each asked for and declared by a test asked for.
plats=()
for p in win32 web; do
  [ "$p" = win32 ] && [ "$want_win32" = 0 ] && continue
  [ "$p" = web ] && [ "$want_web" = 0 ] && continue
  for n in "${names[@]}"; do
    case " $(platforms_of "$GUI/features/$n.sh") " in *" $p "*) plats+=("$p"); break ;; esac
  done
done
[ "${#plats[@]}" -gt 0 ] || { echo "run.sh: none of ${names[*]} declares the platform asked for (--list)" >&2; exit 2; }
uses() { case " ${plats[*]} " in *" $1 "*) return 0 ;; esac; return 1; }

# Its own directory: two runs started in one second are told apart.
LOGS="/tmp/oc-gui/$(date +%Y%m%d-%H%M%S)"
[ -e "$LOGS" ] && LOGS="$LOGS-$$"
mkdir -p "$LOGS" || exit 2
echo "run.sh: logs in $LOGS"

# --- The scripts' shape -------------------------------------------------------------
if ! "$GUI/lint.sh" > "$LOGS/lint.log" 2>&1; then
  cat "$LOGS/lint.log" >&2
  echo "run.sh: a feature script is out of shape (tools/gui/lint.sh, above): nothing was run" >&2
  exit 2
fi

. "$GUI/harness/lock.sh"
gui_lock                  # one GUI run on this machine at a time

# --- What each platform needs, before anything is built ------------------------------
missing=()
if uses win32; then
  command -v powershell.exe >/dev/null || missing+=("win32: powershell.exe (Windows' PowerShell, reached from WSL)")
fi
CHROME_BIN="${CHROME:-$(command -v google-chrome || true)}"
if uses web; then
  command -v node >/dev/null || missing+=("web: node")
  [ -d "$HERE/node_modules/playwright-core" ] || missing+=("web: playwright-core in node_modules (npm install, from package.json)")
  if [ -z "$CHROME_BIN" ]; then missing+=("web: Chrome (CHROME=<path>, or google-chrome on PATH)")
  elif [ ! -x "$CHROME_BIN" ]; then missing+=("web: Chrome at $CHROME_BIN (CHROME), which is not there")
  fi
fi
command -v python3 >/dev/null || missing+=("python3")
if [ "${#missing[@]}" -gt 0 ]; then
  printf 'run.sh: missing: %s\n' "${missing[@]}" >&2
  echo "run.sh: nothing was run" >&2
  exit 2
fi

# --- One build, for every script ------------------------------------------------------
# The daemon with the web client compiled in (WEB=1) and the Windows client,
# as their own make with REMEMBER=0: build/.features, which a later `make
# openchimed` or `make run` reads, is neither read nor rewritten. The switches
# it remembers for read-aloud, voice input, summaries and recording calls are
# passed as they are, so a daemon with speech keeps it and one without is not
# given it.
# remembered <switch>: its remembered value; 0 (off) when there is none.
remembered() { local v; v="$(sed -n "s/^$1=//p" "$HERE/build/.features" 2>/dev/null | head -1)"; echo "${v:-0}"; }
if [ "$build" = 1 ]; then
  # And, for the web, the two helpers features/web_pages.sh presents tokens
  # and mints an invitation with.
  targets="daemon"; uses win32 && targets="$targets windows"; uses web && targets="$targets build/signin build/demo_client"
  BUILD=(make -C "$HERE" REMEMBER=0 TTS="$(remembered TTS)" STT="$(remembered STT)" SUM="$(remembered SUM)" CALLREC="$(remembered CALLREC)" WEB=1 $targets)
  echo "run.sh: building: ${BUILD[*]}"
  if ! "${BUILD[@]}" > "$LOGS/build.log" 2>&1; then
    tail -20 "$LOGS/build.log" >&2
    echo "run.sh: the build failed ($LOGS/build.log): nothing was run" >&2
    exit 2
  fi
fi

# --- What the binaries must be ---------------------------------------------------------
[ -x "$HERE/openchimed" ] || missing+=("the daemon, ./openchimed (make openchimed)")
if uses win32; then
  [ -f "$HERE/build/openchime.exe" ] || missing+=("win32: the Windows client, build/openchime.exe (make build)")
fi
if uses web; then
  for b in build/signin build/demo_client; do
    [ -x "$HERE/$b" ] || missing+=("web: $b (make $b)")
  done
fi
if uses web && [ -x "$HERE/openchimed" ]; then
  # The web client's loader is compiled into the daemon (WEB=1), and its wasm
  # is read from web/ beside it; a daemon built without it serves no client.
  grep -a -m1 ENVIRONMENT_IS_PTHREAD "$HERE/openchimed" >/dev/null && [ -f "$HERE/web/openchime.wasm" ] ||
    missing+=("web: a daemon with the web client in it (make openchimed WEB=1)")
fi
if [ "${#missing[@]}" -gt 0 ]; then
  printf 'run.sh: missing: %s\n' "${missing[@]}" >&2
  echo "run.sh: nothing was run" >&2
  exit 2
fi

# --- Needs ---------------------------------------------------------------------------
# unmet <need>: 0, with the reason in $why, when this machine cannot run a test
# that needs it; 1 when it can.
#   speech   the daemon built with read-aloud and voice input (TTS=1 STT=1) and
#            their data: features/voice.sh renders a sentence and hears it
#            back. Asked as voice.sh asks: a daemon built without them takes
#            neither argument (asked once a run).
#   ffmpeg   ffmpeg and ffprobe, to read back what was recorded
#   callrec  the daemon built with the codecs a call's recording is made with
#            (CALLREC=1): features/calls.sh records a call and plays it back
speech=""
unmet() {
  local out
  case "$1" in
    speech)
      if [ -z "$speech" ]; then
        speech=yes
        out="$(timeout 120 "$HERE/openchimed" --tts-say none none /dev/null 2>&1)"
        out="$out$(timeout 120 "$HERE/openchimed" --stt-hear /dev/null 2>&1)"
        grep 'unknown argument' <<<"$out" >/dev/null && speech=no
      fi
      why="no speech data in ./openchimed"; [ "$speech" = no ] ;;
    ffmpeg)
      why="no ffmpeg"; ! { command -v ffmpeg && command -v ffprobe; } >/dev/null ;;
    callrec)
      # The engine names itself in the binary (daemon/callrec_engine.c).
      why="./openchimed records no calls (make CALLREC=1)"
      ! grep -q 'libvpx-vp9-1280x720@15-450k' "$HERE/openchimed" 2>/dev/null ;;
    *) why="needs $1, which run.sh cannot check"; return 0 ;;
  esac
}

# Take down what a script left running, whatever its own flag: the Windows
# client, every fixture daemon of this checkout (each carries OC_TEST_PORT),
# and every web driver with its browser. Left up, they hold memory the next
# build shares, and a build then crawls under the memory limit.
take_down() {
  "$GUI/harness/win32/drive.sh" kill >/dev/null 2>&1 || true
  local p pids="" i
  for p in $(pgrep -x openchimed 2>/dev/null); do
    [ "$(readlink "/proc/$p/exe" 2>/dev/null)" = "$HERE/openchimed" ] || continue
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -q '^OC_TEST_PORT=' && pids="$pids $p"
  done
  pids="$pids $(pgrep -f "$GUI/harness/web/drive.mjs " 2>/dev/null) $(pgrep -f 'openchime-webdrive/' 2>/dev/null)"
  for p in $pids; do kill "$p" 2>/dev/null; done
  # Gone before the next script starts: asked, then up to 5 s, then killed. A
  # daemon's orderly shutdown takes a moment.
  for i in $(seq 1 50); do
    p=""; for p in $pids; do kill -0 "$p" 2>/dev/null && break; p=""; done
    [ -z "$p" ] && return 0
    sleep 0.1
  done
  for p in $pids; do kill -9 "$p" 2>/dev/null; done
  return 0
}

# --- The runs ---------------------------------------------------------------------------
# cell_<test>_<platform>: what the table shows.
declare -A cell
passed=0; failed=0; stopped=0; skipped=0; interrupted=0
trap 'interrupted=1' INT
# The runs in order, so the last can be left up under --keep.
order=()
for p in "${plats[@]}"; do
  for n in "${names[@]}"; do
    case " $(platforms_of "$GUI/features/$n.sh") " in *" $p "*) order+=("$n:$p") ;; esac
  done
done
web_checked=0
for i in "${!order[@]}"; do
  n="${order[$i]%:*}"; p="${order[$i]#*:}"
  s="$GUI/features/$n.sh"; log="$LOGS/$n-$p.log"
  why=""
  for need in $(needs_of "$s"); do unmet "$need" && break; why=""; done
  if [ -n "$why" ]; then
    cell[$n,$p]="skipped ($why)"; skipped=$((skipped + 1))
    printf '%-14s %-6s %s\n' "$n" "$p" "skipped ($why)"; continue
  fi
  if [ "$p" = web ] && [ "$web_checked" = 0 ]; then
    web_checked=1
    if ! "$GUI/harness/web/driver_check.sh" > "$LOGS/driver_check.log" 2>&1; then
      cat "$LOGS/driver_check.log" >&2
      echo "run.sh: the web driver check failed: no web test was run" >&2
      web_checked=failed
    fi
  fi
  if [ "$p" = web ] && [ "$web_checked" = failed ]; then
    cell[$n,$p]="STOPPED at the web driver check"; stopped=$((stopped + 1)); continue
  fi
  args=(); [ "$keep" = 1 ] && args=(--keep)
  OC_TEST_PLATFORM=$p OC_TEST_NO_BUILD=1 CHROME="$CHROME_BIN" "$s" "${args[@]}" > "$log" 2>&1 < /dev/null
  rc=$?
  # The script's result line (harness/lib.sh, gui_end and its exit trap).
  res="$(grep -E '^gui_[a-z0-9_]+: ' "$log" | tail -1)"; res="${res#*: }"
  nm="$(grep -oE '[0-9]+/[0-9]+ passed' <<<"$res" | head -1)"; nm="${nm% passed}"
  if [[ "$res" == STOPPED* ]]; then
    at="$(sed -n 's/^STOPPED at "\(.*\)": .*/\1/p' <<<"$res")"
    cell[$n,$p]="STOPPED at ${at:-the start}"; stopped=$((stopped + 1))
  elif [[ "$res" == FAILED* ]]; then
    cell[$n,$p]="FAILED $nm"; failed=$((failed + 1))
  elif [ "$rc" = 0 ] && [ -n "$nm" ]; then
    cell[$n,$p]="$nm"; passed=$((passed + 1))
  else
    cell[$n,$p]="STOPPED (exit $rc, no result line)"; stopped=$((stopped + 1))
  fi
  [ "$interrupted" = 1 ] && cell[$n,$p]="STOPPED (interrupted)"
  printf '%-14s %-6s %s\n' "$n" "$p" "${res:-${cell[$n,$p]}}"
  # The last one's clients stay up under --keep; every other's are taken down.
  if [ "$keep" = 0 ] || [ "$i" -lt $((${#order[@]} - 1)) ] || [ "$interrupted" = 1 ]; then take_down; fi
  [ "$interrupted" = 1 ] && break
done

# --- The table ----------------------------------------------------------------------------
w=4; for n in "${names[@]}"; do [ ${#n} -gt $w ] && w=${#n}; done
# value <test> <platform>: its cell; "-" where the test does not declare it.
value() {
  case " $(platforms_of "$GUI/features/$1.sh") " in *" $2 "*) echo "${cell[$1,$2]:-not run}" ;; *) echo - ;; esac
}
declare -A cw
for p in "${plats[@]}"; do
  cw[$p]=${#p}
  for n in "${names[@]}"; do v="$(value "$n" "$p")"; [ ${#v} -gt ${cw[$p]} ] && cw[$p]=${#v}; done
done
echo
printf "%-${w}s" test; for p in "${plats[@]}"; do printf "  %-${cw[$p]}s" "$p"; done; echo
for n in "${names[@]}"; do
  printf "%-${w}s" "$n"
  for p in "${plats[@]}"; do printf "  %-${cw[$p]}s" "$(value "$n" "$p")"; done
  echo
done
echo
echo "run.sh: $passed passed, $failed failed, $stopped stopped, $skipped skipped; logs in $LOGS"
[ "$interrupted" = 1 ] && exit 130
[ $((failed + stopped)) = 0 ]
