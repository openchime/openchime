# The GUI and browser tests

`tools/gui/` drives the real clients -- the Win32 client on this machine's
Windows desktop and the web client in a headless Chromium -- against a real
daemon, and reads what each did from the client's own model dump. Every check
here is run by hand: none runs in `make test` or `make test-all`, and none in
CI (docs/TESTING.md says what the two suites are). Run the GUI smoke before
pushing a client change, and the feature's own test when a change touches it.

## Contents

- [Running](#running)
  - [Prerequisites](#prerequisites)
  - [Needs, and when a test is skipped](#needs-and-when-a-test-is-skipped)
- [The layout](#the-layout)
- [A feature script](#a-feature-script)
  - [What lib.sh gives a script](#what-libsh-gives-a-script)
  - [What lint.sh holds a script to](#what-lintsh-holds-a-script-to)
- [The variables](#the-variables)
- [One GUI run at a time](#one-gui-run-at-a-time)
- [The tests](#the-tests)
- [Driving the web client](#driving-the-web-client)
- [Driving the Win32 GUI](#driving-the-win32-gui)
- [Reading the startup harness](#reading-the-startup-harness)
- [Reading the New message harness](#reading-the-new-message-harness)
- [Reading the GUI smoke](#reading-the-gui-smoke)
- [Reading the screen-recording harness](#reading-the-screen-recording-harness)
- [Reading the links harness](#reading-the-links-harness)
- [Reading the reactions harness](#reading-the-reactions-harness)
- [Reading the navigation-history harness](#reading-the-navigation-history-harness)
- [Reading the mentions harness](#reading-the-mentions-harness)
- [Reading the polls harness](#reading-the-polls-harness)
- [Reading the tables harness](#reading-the-tables-harness)
- [Reading the shortcuts harness](#reading-the-shortcuts-harness)
- [Reading the composer harness](#reading-the-composer-harness)
- [Reading the drafts harness](#reading-the-drafts-harness)
- [Reading the files harness](#reading-the-files-harness)
- [Reading the lightbox harness](#reading-the-lightbox-harness)
- [Reading the uploads harness](#reading-the-uploads-harness)
- [Reading the members harness](#reading-the-members-harness)
- [Reading the keyboard-menu harness](#reading-the-keyboard-menu-harness)
- [Reading the calls harness](#reading-the-calls-harness)
- [Reading the search-box harness](#reading-the-search-box-harness)
- [Reading the voice harness](#reading-the-voice-harness)
- [Reading the cursor harness](#reading-the-cursor-harness)
- [Seeing the whole window](#seeing-the-whole-window)
- [`clipped=`: text that does not fit its own box](#clipped-text-that-does-not-fit-its-own-box)
- [Observing the tray balloon](#observing-the-tray-balloon)
- [Reading the jump-to-date harness](#reading-the-jump-to-date-harness)
- [Reading the snippets harness](#reading-the-snippets-harness)
- [Reading the web pages harness](#reading-the-web-pages-harness)
- [Reading the web client harness](#reading-the-web-client-harness)

## Running

    tools/gui/run.sh                     # every test, on every platform it declares: Windows, then the web
    tools/gui/run.sh smoke calls         # those tests, on every platform each declares
    tools/gui/run.sh --web smoke calls   # ... in the browser only
    tools/gui/run.sh --win32 smoke       # ... on Windows only
    tools/gui/run.sh --list              # each test, its platforms and what it needs
    tools/gui/run.sh --keep smoke        # the last test's clients and daemon are left running
    tools/gui/run.sh --no-build ...      # test the binaries already built

A test is named as its script is, with or without `.sh`
(`tools/gui/features/<name>.sh`). Each declares the platforms it runs on in its
header (`# platforms: win32 web`), and what it needs beyond them (`# needs:
ffmpeg`); `run.sh` keeps no list of tests and reads only those lines. A run, in
order:

1. holds every feature script to its shape (`tools/gui/lint.sh`, below), and
   runs nothing if one is out of it;
2. takes the machine's GUI lock ([One GUI run at a time](#one-gui-run-at-a-time)),
   held to its end;
3. checks each platform it will use for its prerequisites (below), names what is
   missing, and runs nothing if anything is;
4. builds once (unless `--no-build`): the daemon with the web client in it
   (`WEB=1`), the Windows client for a Windows run, and `build/signin` and
   `build/demo_client` for a web run, with the read-aloud, voice input and
   summary switches as `build/.features` remembers them (`REMEMBER=0`: that file
   is neither read for anything else nor rewritten). The scripts themselves then
   build nothing (`OC_TEST_NO_BUILD=1`). The binaries the run needs are then
   checked for, and a daemon without the web client in it is named;
5. runs the web driver check (`tools/gui/harness/web/driver_check.sh`) before
   the first web test; if it fails, no web test runs, and each is shown
   stopped at it;
6. runs each test on each platform, Windows first, one at a time, and takes
   down whatever it left running (its Windows client, its fixture daemon, its
   browser) before the next. Under `--keep` only the last one's are left up.

Each run's logs go to `/tmp/oc-gui/<date>-<time>/`: `lint.log`, `build.log`,
`driver_check.log` and one `<test>-<platform>.log` per test per platform, each
the script's whole output. One line per test is printed as it ends, and one
table at the end, a row per test and a column per platform:

| Cell | Means |
|---|---|
| `N/M` | passed: all of its M checks |
| `FAILED N/M` | it ran to its end, and M-N checks failed |
| `STOPPED at <step>` | it stopped in that step: a fixture that did not come up, a check the rest depended on, or a verb the client did not answer (`STOPPED (interrupted)` for Ctrl+C) |
| `skipped (<why>)` | this machine does not meet one of its needs |
| `-` | the test does not declare that platform |

**Exit status.** 0 when nothing failed or stopped (a skipped test is neither
passed nor failed, and is never counted as passed); 1 when anything failed or
stopped; 2 when the run could not start (a script out of shape, a missing
prerequisite, a failed build, an unknown test); 130 when interrupted.

A feature script also runs on its own, `tools/gui/features/<name>.sh [--keep]`,
on Windows unless `OC_TEST_PLATFORM=web` says the browser; run so, it builds what
its driver builds (`make daemon` and the Windows client) unless
`OC_TEST_NO_BUILD=1`.

### Prerequisites

**Both platforms:** `python3`, and a Linux machine to run the daemon (it is
epoll-based).

**Windows** (`--win32`): WSL2 on a Windows host with an interactive desktop
session, `powershell.exe` reachable from WSL and `/mnt/c` mounted. The run
builds the client (`make windows`).

**The web** (`--web`): `node` with the `playwright-core` package (`npm install`,
from `package.json`; the web driver installs it when it is missing), and Chrome:
the one `CHROME=/path` names, else the installed `google-chrome` on `PATH`
(nothing is downloaded). Building the web client needs `git`: make fetches
Emscripten, about 400 MB, and its Node.

`run.sh` checks each of these for the platforms a run uses, and names whatever
is missing.

### Needs, and when a test is skipped

A need is what a test requires beyond its platform, on its `# needs:` line;
`run.sh` checks each before the test, and skips the test, saying why, when this
machine does not meet one:

| Need | Met when | Declared by |
|---|---|---|
| `speech` | `./openchimed` was built with read-aloud and voice input and their data (`TTS=1 STT=1`): it takes `--tts-say` and `--stt-hear` | `voice` |
| `ffmpeg` | `ffmpeg` and `ffprobe` are on `PATH`, to read back what was recorded | `screenrec` |
| `callrec` | `./openchimed` was built with the codecs a call's recording is made with (`CALLREC=1`); `run.sh` passes the remembered `CALLREC` to its build | `calls`, `callhome` |

A need `run.sh` does not know is a skip too ("needs X, which run.sh cannot
check"), never a pass. A test whose own fixture cannot come up on this machine
stops (`STOPPED at ...`) rather than skipping: a skip is only for a need
declared in advance.

## The layout

    tools/gui/
      run.sh            the runner (above)
      lint.sh           the feature scripts' shape, with its own self-test
      README.md         this
      harness/          what the scripts are written over
        lib.sh          sourced by every feature script: the platform, its drivers, the helpers
        lock.sh         gui_lock, the machine-wide lock
        win32/          the Windows client
          drive.sh      one client: launch, verbs, dump, shot, kill
          pair.sh       two clients, alice and bob, on one daemon
          win.sh        every call into Windows' PowerShell, by name
          *.ps1         the PowerShell scripts win.sh runs
        web/            the web client
          drive.sh      one client in a headless Chromium, the same verbs
          drive.mjs     its browser half (Node, playwright-core)
          pair.sh       two clients
          driver_check.sh  one driver per client, checked with stand-ins
          pages.mjs, invite.mjs, load.mjs   the daemon's pages from a browser (web_pages)
      features/         one script per feature: the tests

**harness/win32/.** `drive.sh` starts the Win32 client with its test hook on
(`OPENCHIME_DEV_DIR`, a file command channel on the Windows share) and sends
it verbs; `pair.sh` does the same for two. Whatever the harness asks of
Windows itself goes through `win.sh`: closing the client and finding its
processes (`win_close_client`, `win_client_pids`, `win_stop_pids`), arranging
a pair's windows (`win_arrange_windows`), forgetting the fixtures' saved
sign-ins in Credential Manager (`win_forget_signins`, `win_forget_signin`),
setting and reading the clipboard as another program does
(`win_clipboard_set_text`, `_set_files`, `_set_png`, `_holds`, `_read`,
`_clear`), starting and stopping another program (`win_open_program`,
`win_stop_program`), playing the person's browser on Windows
(`win_browser_post`, over `browser.ps1`), UI Automation (`win_uia_invoke` over
`uia_invoke.ps1`, `win_uia_recbar` over `uia_recbar.ps1`; `uia_probe.ps1`
walks the whole tree, by hand), and capturing the desktop (`win_capture_desktop`,
over `balloon.ps1`). PowerShell is needed because those things are Windows':
the client's processes and windows, its clipboard, its saved sign-ins, its
UI Automation tree, the client's loopback for the browser sign-in and the
composited desktop are reachable from Windows and not from WSL. Each start of
`powershell.exe` costs a second or more, so each helper does its whole job in
one. No feature script carries PowerShell of its own.

**harness/web/.** `drive.sh` and `pair.sh` are the Windows ones' twins for the
browser (["Driving the web client"](#driving-the-web-client)).
`driver_check.sh` is run by `run.sh` before any web test. `pages.mjs`,
`invite.mjs` and `load.mjs` drive the daemon's own pages, not the client, and
are run by `features/web_pages.sh`.

**features/.** One script per feature, each a test of its own, run by
`run.sh` or alone.

## A feature script

Every script under `tools/gui/features/` is written one way, with what
`tools/gui/harness/lib.sh` gives it (`features/jumpdate.sh` is the shortest
whole one):

    #!/usr/bin/env bash
    # What it shows, and how to run it.
    #
    # platforms: win32 web
    # needs: ffmpeg
    set -uo pipefail
    HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
    . "$HERE/tools/gui/harness/lib.sh"
    gui_begin <name> <port> <dir> [workspace] "$@"
    step "what this part shows"
    gui_launch                    # one client, signed in; or gui_pair_up for two
    check "what is true" waitfor 5 <predicate>
    gui_end

- `gui_begin` takes down whatever an earlier run left on the script's port
  (its pair, its client, its daemon), wipes its directory (not with
  `OC_TEST_KEEP_DB=1`), and from then on stops all of it when the script
  exits: passed, failed, stopped or interrupted. `--keep` leaves the clients
  and the daemon running, to look at afterwards. Each script has its own port
  (9500-9799; `grep -n gui_begin tools/gui/features/*.sh` shows the ones taken)
  and its own directory under `/tmp`.
- `gui_launch` and `gui_pair_up` return signed in, or stop the script with the
  card's state: nothing after a fixture that did not come up could say anything
  about the feature. `gui_abort "<why>"` stops it the same way at any other
  fixture step, and `need` is a check the rest depends on.
- A wait is `waitfor <secs> <predicate>`, the predicate reading a fresh dump
  (`gui_snap`, or `gui_snap a|b` in a pair, into `$D`, `$DA`, `$DB` in the
  script's own directory; `field <file> <line> <key>` reads a value). A
  `sleep` is a pause that is itself under test and says so: `# paced: <why>`.
- `step "<part>"` heads each part, and a script that stops names the part it
  stopped in. `gui_end` prints the one result line, `gui_<name>: N/M passed in
  Ss` (or `FAILED: ...`, or `STOPPED at "<part>": ...`), and sets the status
  `run.sh` counts.
- A poll writes the dump to a file and greps the file, never `| grep -q`:
  the scripts run under `pipefail`, and `-q` quits at its first match, which
  leaves the dump's writer with a closed pipe and the pipeline failing on the
  match it found.

### What lib.sh gives a script

| Helper | Does |
|---|---|
| `gui_begin`, `gui_end`, `step`, `gui_abort` | the shape above |
| `GUI_AT_EXIT=<command>` | run on the way out (not with `--keep`), while the client is still up: to undo what the script left outside its own directory (`web_signin.sh` forgets its workspace) |
| `check <what> <command...>`, `need <what> <command...>` | a check, said `ok` or `FAIL`; `need` stops the script when it fails |
| `say`, `ok`, `fail` | a line; a passed or failed check by hand |
| `waitfor <secs> <command...>` | the command, four times a second, until it succeeds |
| `gui_snap [a\|b]` | the client's dump into `$D` (a pair's into `$DA`, `$DB`) |
| `seen <a\|b> <command...>`, `dfile <a\|b>` | a fresh dump of that pair client, then the command; that client's dump file |
| `field <file> <line-ERE> <key>`, `fieldq ...` | a `key=` value on the first matching dump line; `fieldq` for a quoted value with spaces |
| `act [a\|b] <verb...>` | a verb, the script stopped when the client does not answer it |
| `ack_of [a\|b] <verb...>` | the word the verb was acked with (`ok`, `err` ...) |
| `click_rect [a\|b] <l,t,r,b>`, `aid_xy <file> <aid>`, `press [a\|b] <aid>` | a click at a dump rect's centre; where a published (`a11yitem`) control is; a control pressed as a person presses it, once it is published |
| `cpath <name>`, `make_png <file> <w>x<h> <rrggbb> [<rrggbb>]` | a file's path as the client names it (`$LIN_DIR`, `$WIN_DIR`); a PNG of one or two colours |
| `bmp_px <file.bmp> <x> <y> [n]` | pixel colours in a screenshot the client wrote |
| `newest_is`, `send_stored`, `upload_stored` | a conversation's newest message; a message or a file sent and waited for back from the daemon |
| `sign_in_card`, `play_browser` | the Win32 sign-in card driven as a person does, and the daemon's page played for them from Windows |
| `kill_dev_daemon`, `dev_daemon_pids` | the fixture daemon on `$OC_TEST_PORT`, found by its environment, and stopped |
| `$PLAT`, `$DRIVE`, `$PAIR` | the platform, and its one-client and pair drivers |

### What lint.sh holds a script to

`tools/gui/lint.sh` runs first in every `run.sh`, and by hand as it is. Each
feature script:

- **shape**: sources `tools/gui/harness/lib.sh`, calls `gui_begin`, and ends
  with `gui_end`;
- **own**: defines none of what `lib.sh` gives it, handles no `--kill` or
  `--down` of its own, and prints no result line of its own;
- **sleep**: calls `sleep` only where the pause is itself under test, and says
  so on the line or the line before (`# paced: <why>`);
- **platforms**: declares its platforms in its header, `win32` and `web` only.

And across `tools/gui/`, every `OC_TEST_*` name set or used is read somewhere
there: a name nothing reads is a setting that does nothing. It prints
`file:line: rule: text` for each, and exits 1 on any. Before the tree, it runs
on a stand-in tree that breaks every rule and one that keeps them, and fails
unless it reports each break and nothing else.

## The variables

The harness's variables begin `OC_TEST_` (but `GUI_DRIVE_OUT`, the output
directory), and are read and defaulted in
`tools/gui/harness/lib.sh` (the feature scripts), `tools/gui/harness/win32/drive.sh` and
`tools/gui/harness/web/drive.sh` (one client) and the two `pair.sh` (two clients); an older
prefix in the environment (`OC_DEV_`, `OC_PAIR_`, `OC_GUI_`, `OC_DRIVE_`,
`OC_WEB_`, `OC_SMOKE_`) is refused by name. The daemon and the clients they start run in
development mode (`tools/gui/harness/lib.sh` exports `OPENCHIME_MODE=development`,
CONFIG.md "Mode"), and any build will do.

| Variable | Default | Meaning |
|---|---|---|
| `OC_TEST_PLATFORM` | `win32` | `win32` or `web`: which client the feature scripts drive (`run.sh` sets it for each run; `web_pages` and `web_client` set `web` themselves). |
| `OC_TEST_PORT` | 8443 (one client), 9600 (a pair) | The fixture daemon's protocol port; every feature script sets its own. |
| `OC_TEST_DIR` | `/tmp/openchime-dev`, `/tmp/openchime-pair` | The fixture daemon's state directory, wiped per run. |
| `OC_TEST_WS` | `Acme HQ` | The fixture's workspace name. |
| `OC_TEST_USERS` | `alice,bob,carol` / `alice,bob` | The fixture's bootstrapped users (`name:pw:role`). |
| `OC_TEST_HOST` | the WSL machine's address / `127.0.0.1` | Where the clients reach the daemon. |
| `OC_TEST_CALL_MAX`, `OC_TEST_ADVERTISE_PORT` | 10, unset | The fixture's call size and advertised UDP port. |
| `OC_TEST_TONE_A`, `OC_TEST_TONE_B` | unset | Each paired client's synthetic microphone tone, in Hz. |
| `OC_TEST_NO_BUILD`, `OC_TEST_NO_DAEMON`, `OC_TEST_LOCAL` | 0 | Skip the build (`run.sh` sets it, having built once itself); leave the daemon alone; run the Windows client from a local copy. |
| `OC_TEST_DRIVE`, `OC_TEST_PAIR` | by platform | A driver of one's own, for a one-off. |
| `OC_TEST_WHO`, `OC_TEST_DEBUG_PORT` | `a`, unset | The web driver's client letter; Chromium's remote-debugging port. |
| `OC_TEST_VIEWPORT`, `OC_TEST_DPR` | `1280x800`, 1 | The web client's window (the browser's viewport) and its device pixels per CSS pixel: what `size` and `dpi` set on Windows. |
| `CHROME` | `google-chrome` on `PATH` | The Chrome or Chromium the web driver and `web_pages` run. |
| `OC_TEST_HEALTH_PORT`, `OC_TEST_WEB_PORT` | 8080, 8551 | The fixture daemon's health port; the fixture daemon's port in `tools/gui/features/web_signin.sh` (the Win32 browser sign-in). |
| `OC_TEST_KEEP_DB`, `OC_TEST_WAIT_MS` | 0, the script's | A feature script keeps its database rather than wiping it; the smoke's longer wait for a slow machine. |
| `GUI_DRIVE_OUT` | `/tmp/ocshot` (`/tmp/ocshot-web` in `web_signin.sh`) | Where `shot`/`dump` copies land on the Linux side. |
| `OC_TEST_GUI_LOCK`, `OC_TEST_GUI_LOCKED` | `/tmp/openchime-gui.lock`, unset | The machine-wide lock ([One GUI run at a time](#one-gui-run-at-a-time)); set once it is held, so nested scripts do not wait on themselves. |

The test binary's own variables share the prefix and are not the harness's:
`OC_TEST_ONLY`, `OC_TEST_EXCEPT`, `OC_TEST_REPEAT`, `OC_TEST_JOBS`
(`tests/main.c`) and `OC_TEST_TIMING` (`tests/check.h`), in docs/TESTING.md.

## One GUI run at a time

The scripts share one Windows
desktop, its clients and its saved development sign-ins, from whatever checkout
they run: a second run's `drive.sh kill` stops the first's clients, and two
sign-ins at once slow each other. So `tools/gui/run.sh`, every script through
`tools/gui/harness/lib.sh`, and `pair.sh up` take one lock (`tools/gui/harness/lock.sh`,
`flock` on `OC_TEST_GUI_LOCK`) before they touch any of it and hold it to their
end; a run that finds it taken says `gui_lock: another GUI run is going` and
waits for it. The lock is a file descriptor, and a process started in the
background inherits it, so a daemon or client left running would hold it after
its run ended and every later run would wait for good: everything the harness
starts in the background is started with it closed (`9>&-`).

## The tests

Each test's platforms and needs are its header's (`tools/gui/run.sh --list`
prints them); its port is its own fixture daemon's. The notes are what to know
when reading its output.

| Test | Platforms | Needs | Port | What it checks | Notes |
|---|---|---|---|---|---|
| `admin_rows` | win32 | | 9565 | the audit log's rows: a menu each, by right-click and from the keyboard | its header |
| `balloon` | win32 | | 9786 | the tray balloon is drawn on the desktop | [Observing the tray balloon](#observing-the-tray-balloon) |
| `calls` | win32 web | | 9620 | a call between two clients, its sound both ways, sharing a screen | [Reading the calls harness](#reading-the-calls-harness) |
| `calls_tcp` | win32 web | | 9670 | a call when UDP cannot reach the relay | [Reading the calls harness](#reading-the-calls-harness) |
| `camera` | win32 web | | 9676 | cameras in the call view, both ways | [Reading the calls harness](#reading-the-calls-harness) |
| `chords` | web | | 9750 | a shortcut pressed quickly in the browser is still the shortcut | [Driving the web client](#driving-the-web-client) |
| `collapse` | win32 web | | 9700 | long messages collapsed by the reader | its header |
| `composer` | win32 | | 9540 | the composer's layout | [Reading the composer harness](#reading-the-composer-harness) |
| `cursor` | win32 web | | 9505 | the mouse cursor's shape, read from the client | [Reading the cursor harness](#reading-the-cursor-harness) |
| `drafts` | win32 | | 9560 | deleting a draft from Drafts | [Reading the drafts harness](#reading-the-drafts-harness) |
| `emoji_admin` | win32 web | | 9525 | Admin > Custom emoji | its header |
| `files` | win32 web | | 9590 | paging the Files view | [Reading the files harness](#reading-the-files-harness) |
| `groups` | win32 | | 9640 | user groups, two clients | [Reading the calls harness](#reading-the-calls-harness) (its last part) |
| `jumpdate` | win32 | | 9760 | jump to a date | [Reading the jump-to-date harness](#reading-the-jump-to-date-harness) |
| `keymenu` | win32 | | 9610 | the keyboard's route to the context menu | [Reading the keyboard-menu harness](#reading-the-keyboard-menu-harness) |
| `lightbox` | win32 web | | 9710 | the expanded image view pages through the conversation's media | [Reading the lightbox harness](#reading-the-lightbox-harness) |
| `links` | win32 | | 9650 | links in the transcript | [Reading the links harness](#reading-the-links-harness) |
| `members` | win32 | | 9690 | scrolling the members pane | [Reading the members harness](#reading-the-members-harness) |
| `mentions` | win32 web | | 9730 | completing a mention, a channel and a pick from the list | [Reading the mentions harness](#reading-the-mentions-harness) |
| `navhistory` | win32 web | | 9672 | Back and Forward | [Reading the navigation-history harness](#reading-the-navigation-history-harness) |
| `newmsg` | win32 | | 9520 | the New message pane | [Reading the New message harness](#reading-the-new-message-harness) |
| `pdm` | win32 web | | 9515 | encrypted DMs, two clients | its header |
| `permalink_w` | win32 | | 9674 | an email's link opened in the client | its header |
| `polls` | win32 web | | 9770 | polls made, changed and voted in | [Reading the polls harness](#reading-the-polls-harness) |
| `preview` | win32 web | | 9535 | the composer's Preview | its header |
| `reactions` | win32 web | | 9580 | reacting from the who-reacted pane | [Reading the reactions harness](#reading-the-reactions-harness) |
| `screenrec` | win32 web | `ffmpeg` | 9530 | screen recording, sent and stored | [Reading the screen-recording harness](#reading-the-screen-recording-harness) |
| `search_boxes` | win32 web | | 9660 | the six search boxes, one control | [Reading the search-box harness](#reading-the-search-box-harness) |
| `shortcuts` | win32 | | 9570 | the keyboard shortcuts, by their effect | [Reading the shortcuts harness](#reading-the-shortcuts-harness) |
| `smoke` | win32 web | | 9500 | the client boots and runs; the check before pushing a client change | [Reading the GUI smoke](#reading-the-gui-smoke) |
| `snippets` | win32 | | 9720 | snippets posted and shown as cards | [Reading the snippets harness](#reading-the-snippets-harness) |
| `spoiler` | win32 web | | 9555 | spoilers hidden until a reader chooses | its header |
| `startup` | win32 | | 9510 | what the client does between launch and its first frame | [Reading the startup harness](#reading-the-startup-harness) |
| `tables` | win32 web | | 9550 | Markdown tables in the transcript and the composer | [Reading the tables harness](#reading-the-tables-harness) |
| `timetoken` | win32 web | | 9545 | time tokens, read in each reader's zone | its header |
| `todos` | win32 web | | 9740 | to-do lists in messages | its header |
| `uploads` | win32 web | | 9630 | the upload tray in the message box | [Reading the uploads harness](#reading-the-uploads-harness) |
| `voice` | win32 web | `speech` | 9680 | voice input, with real recognition | [Reading the voice harness](#reading-the-voice-harness) |
| `web_client` | web | | 9585 | what only the browser client has: a video message in wasm, the text layer, About, the input method | [Reading the web client harness](#reading-the-web-client-harness) |
| `web_pages` | web | | 9595 | the daemon's sign-in pages, an invitation link and the client's load, from a real browser | [Reading the web pages harness](#reading-the-web-pages-harness) |
| `web_signin` | win32 | | 8551 | local accounts signing in in the browser, from the Win32 client | its header; docs/TESTING.md §3.1 |
| `whatsnew` | win32 | | 9780 | What's new: this build's release notes | its header |
| `workspaces` | win32 web | | 9790 | the Workspaces dialog | its header |

## Driving the web client

`tools/gui/harness/web/drive.sh` is `tools/gui/harness/win32/drive.sh` for the browser: the same
subcommands (`launch`, `devaddr`, `<verb...>`, `shot`, `dump`, `kill`), the
same verbs, the same ack contract (exit status 1 when the client did not
answer), and `tools/gui/harness/web/pair.sh` is `tools/gui/harness/win32/pair.sh` (`up`, `a`, `b`, `down`).
A feature script therefore runs against either client: every script sources
`tools/gui/harness/lib.sh`, which picks the drivers from `OC_TEST_PLATFORM=win32|web`
(win32 unless said) and sets `PLAT` for the checks that are the Windows
platform's alone. `tools/gui/run.sh` runs each script on the platforms its
header declares (Running, above).

**One driver per client.** Two drivers serving one command directory race for
each verb, so a run fails at random with nothing wrong in the client. `kill`
ends every driver serving the directory, found by the command line that names
it, and the browser it held. It asks first (`quit`), then sends SIGTERM, then
SIGKILL to whatever is still running 3 seconds later, and says how many it had
to force; a driver ends on SIGTERM or SIGINT by closing its browser. `launch`
runs `kill`, then refuses to start, naming the pids, if `held` still finds a
driver on the directory. `tools/gui/harness/web/driver_check.sh`, which
`run.sh` runs before the first web test, checks both with stand-in drivers
that ignore SIGTERM.

**How it drives.** `launch` builds and starts the fixture daemon as
`tools/gui/harness/win32/drive.sh` does (`OC_TEST_PORT`, `OC_TEST_DIR`, `OC_TEST_WS`,
`OC_TEST_USERS`; the health port off), then starts `tools/gui/harness/web/drive.mjs`: a
Node process holding a headless Chromium on the daemon's own `/app/` page,
signed in with the password quick-launch and the test hook on. The shell side
writes the verb to `$OC_TEST_DIR/web-<a|b>/cmd` and waits for `ack`, as it
does with the Windows client's scratch directory; the Node side relays the
verb to the page's hook, takes a `dump` or `shot` back out of the page's
filesystem into that directory, and copies an `attach`ed file onto the page
first so the verb names a path the client can read; the shell side waits up
to 5 minutes for those verbs, rather than 16 seconds, since the copy takes as
long as the file is large. One verb is the
browser's own: `press <chord>` (`ctrl+comma`, `ctrl+shift+slash`, `esc`)
dispatches the page's key events for the chord, every press and release in
one task, so the client receives them as one batch, the way a quick
person's chord arrives between two frames. `key` hands the client a chord
directly; `press` goes the way a keyboard does, through the page and SDL.
`tools/gui/features/chords.sh` (port 9750, web only) presses Ctrl+, and Ctrl+/
several times each and checks Preferences and the shortcuts open every
time: the client must read a key's modifiers from the key event, not from
the keyboard's state when it gets to the key, by which time a quick
Ctrl is already up. `press` acks once the events are dispatched, and the
script polls the dump for the state each chord leads to, as the smoke's
`wait_for` does. Three more are the browser's clipboard, as
another program leaves and reads it, rather than the page's own store that
`clipset` and `paste` reach: `sysclip text <words>` (escaped as `clipset`)
and `sysclip png <path>` put text or a picture on it, `syspaste` presses
Ctrl+V through Playwright's keyboard so the browser itself pastes, and
`sysclipread` acks `ok <text>` with what it holds. `tools/gui/features/uploads.sh`
uses them on the web: a picture and then text copied outside the client
paste into the box, Ctrl+C leaves the box's text on the clipboard, and a
message's Copy text -- reached from the keyboard, Ctrl+Down, Shift+F10 and
the highlight walked to it -- leaves the message there. The browser runs with
Chromium's fake camera and microphone, its own permission prompts answered,
its screen picker choosing the screen; `OPENCHIME_DEV_AUDIO`, `_MIC`,
`_CAPTURE`, `_TONE` and the other switches reach the page as `?env=`, and a
`_MIC` WAV is served to it. Needs `node`, the `playwright-core` package
(`package.json`; `launch` installs it) and Chrome (`CHROME`, else the
installed `google-chrome`).

**What the ports skip in a browser**, by `PLAT`: UI Automation (`uia_*.ps1`),
the recording bar's own window and its capture exclusion, a window as a
recording source (the browser's picker is its own), the clipboard set from
PowerShell (the web run sets the browser's with `sysclip` instead), and the tray's notification counters. Everything else -- the
dumps, the verbs, the media checks down to the tones in the recorded file --
is the same code.

## Driving the Win32 GUI

`tools/gui/harness/win32/drive.sh launch` **builds both sides and restarts the daemon if the
running one predates the binary it just built.** That is not convenience: the
client and daemon share a wire and ship together (ARCH-61), so a client built
from source N against a daemon still running N-1 decodes garbage and reports only
"connection lost — reconnecting", which points nowhere near the cause. It cost
real time three times in a single day before the guard existed.

`make` is incremental, so it is free when nothing changed. `OC_TEST_NO_BUILD=1`
skips the build and `OC_TEST_NO_DAEMON=1` leaves the daemon alone — for when a
mismatched pair is the thing under test, such as the version-reject path.

**A pair comes up or says why not.** `pair.sh up` waits on what it can see --
the daemon listening (or its process gone, with its log's last lines), each
client answering, its card's sign-in, `authed=1` -- and never on a clock or a
count of looks. The Win32 sign-in sets the card's workspace itself (`siws`)
before step 1, so what is submitted is what was meant, and submits step 1 again
when it comes back with an error, up to five times. A client that stops
answering, or whose sign-in shows an error, fails `up` with `pair: a client did
not sign in`, after its pids are recorded so `down` still stops them; a script's
`gui_pair_up` then stops it at its fixture (`STOPPED`), not at its features. `down` sends `quit` only to a client that is
running. The web pair launches both pages at once.

**`devenv OPENCHIME_DEV_<NAME> <value>`** changes one development input in the
running client (development mode, those names only), so a script tests a
refused camera, say, in the pair it has rather than starting another.

**The client is pointed at the address Windows reaches the daemon at.** Under
WSL2 that is the WSL machine's own address (eth0's), not 127.0.0.1, which reaches
a WSL listener only where localhost forwarding is on; elsewhere it is 127.0.0.1,
and `OC_TEST_HOST` overrides either. `tools/gui/harness/win32/drive.sh devaddr` prints the workspace
`launch` defaults to, for a script that names it. At an address that is not
loopback the dev daemon's self-signed certificate is asked about (ARCH-10), and
`launch` answers Trust and connect for it — the daemon is the one the harness
started — before it returns. It starts the client only once the daemon listens,
waiting up to ninety seconds: the bootstrap accounts are hashed first, which for
`tools/gui/features/members.sh`'s sixty people is half a minute, and a client whose first attempt
finds nothing there does not try again. Only the daemon on `OC_TEST_PORT` — found
by its environment — is restarted.

**Video messages drive on a synthetic camera.** Launch with
`WSLENV=OPENCHIME_DEV_CAPTURE:OPENCHIME_DEV_AUDIO OPENCHIME_DEV_CAPTURE=synthetic
OPENCHIME_DEV_AUDIO=synthetic tools/gui/harness/win32/drive.sh launch` (add
`OPENCHIME_DEV_VIDEO_CAP_MS` to shorten the five-minute cap, or
`OPENCHIME_DEV_CAPTURE=denied` for the blocked-camera path). The `vm` verb opens,
records, stops, sends, plays and closes by name, and `dump` reports the overlay on
its `vm=` line. The synthetic source never opens a real camera, so a run cannot
turn on the camera of the machine it happens to be on.

**A real camera needs the client on a local disk.** Windows refuses camera
activation to an executable started from the WSL share (`\\wsl.localhost\…`):
`ActivateObject` fails with `0x80070490`, element not found, and the card says
the recording could not start, with that step in brackets. `tools/gui/harness/win32/drive.sh launch`
runs `build/openchime.exe` from the share, which is fine for everything else;
for a camera test, copy it to a Windows directory and start it from there —
`OC_TEST_LOCAL=1` makes `launch` do exactly that.

**Two verbs put text in the composer, and they are not interchangeable.**
`type` sets the buffer directly (`ed_set`), bypassing the editor's own rules;
`typekeys` sends each character as a real `WM_CHAR` through the editor, so the
rich-mode typing rules (pending styles, continuation, delimiter handling) run
exactly as they do for a user. Testing an editor behaviour with `type` proves
nothing about typing.

**`key` presses a key down; it does not let it go.** The release is its own verb
(`keyup`), so a harness can hold a key — push to talk needs exactly that. The cost
is that a handler which re-arms on the release sees the first press and then
nothing: Backspace in the To: field takes one recipient per press and clears its
latch in `WM_KEYUP`, so a script that never released it deleted one chip and
silently did nothing on every Backspace after, while the checks that followed
asserted against a state the run never reached. **Anything pressed more than once
in a run must be released**, which is what `tools/gui/features/newmsg.sh`'s `press` helper does:
`drive key <k>; drive keyup <k>`. Hold a key only where the hold is the thing
under test.

> **The smoke owns its own daemon.** It defaults to
> port **9500** and `/tmp/oc-smoke`, wipes that directory, and **verifies the
> workspace it reached is the fixture** — name plus the presence of alice, bob
> and carol — refusing to run otherwise. Silently adopting whatever daemon is
> listening means asserting fixture users against somebody's real workspace and
> reporting confident nonsense, which is exactly what the fixture check refuses.
>
> **Kill a dev daemon by its environment, not its command line.** It is started
> as `env OPENCHIME_PORT=… openchimed`, so the port never appears in the
> process's *cmdline* and `pkill -f OPENCHIME_PORT=9500` matches nothing —
> exiting 1, looking exactly like "nothing to kill". The daemon then keeps
> running on a directory that has been deleted underneath it (SQLite happily
> writes to the unlinked inode), and every upload fails with an opaque
> `transfer error`. Match `/proc/<pid>/environ`, as both harnesses do.

## Reading the startup harness

`tools/gui/features/startup.sh` answers the question the smoke suite deliberately
stops short of: **what does the client do between launch and its first frame.**
It launches the client four ways — a workspace that signs in, one that does not
resolve (`nosuch.invalid`, reserved never to), one that is malformed, and one
that resolves but where nothing listens — and asserts on the client's own dump
for each: that the window is **visible**, that **exactly one** client started,
which view it landed on, and that a failure **says why**. Fifteen checks, on its
own fixture daemon and port (9510).

It exists because two defects went out through that stretch and were caught
only by a person noticing: a workspace that did not resolve left a running
process with an invisible window and no message, and the fix for it started the
client twice. The dump's `startup` line (`visible= started= si_ws= si_err=`) is
what makes both assertable. It was proved to catch both: with the double start
put back and the sign-in reason dropped, four of its checks fail, each naming
the defect.

It is Win32-only and not in CI (the smoke's Win32 reason), and it is not the
pre-push gate — run it when a change touches how the client starts.

## Reading the New message harness

`tools/gui/features/newmsg.sh` asks the questions the New message pane (REQ-229)
answered wrongly for its whole life, in 32 checks on its own fixture daemon and
port (9520):

- **Does the field that looks focused get the keys?** Characters, and the keys
  that are not characters — Delete, Ctrl+A, Ctrl+V — while the To: field has
  focus. Falling through to the message body behind it would mean Ctrl+V
  pastes into a message nobody can see and Ctrl+A then Delete wipes it.
- **Does the message go where the pane says?** It posts into `#general` first, so
  a wrong send has somewhere visible to land, then addresses a message to bob and
  presses Enter. `#general`'s message count must not move and the DM's must.
- **Does what you typed survive?** Leaving and returning must bring back the words
  **and** the recipients, and the pane's text must never become the selected
  conversation's draft.
- **Does Backspace take one recipient per press?** One press takes one; the key
  held — pressed again without a release — takes no more, which is the rule that
  stops a held key walking backwards through the whole list.
- **Does a send with nobody addressed refuse, and say so?** The recipients being
  gone is asserted *before* Enter is pressed, the message must still be in the box
  afterwards, and the composer must say why (the dump's `hint` line). The harness releases Backspace between
  presses (see the `key`/`keyup` rule above); otherwise the check would run with a
  recipient still attached.

It reads the dump's `newmsg` line — `focus= chips= q= caret= sel= matches= body=
pending=` — which exists so those states can be asserted rather than described.
Both halves of the pane are in it: which field owns the keyboard, who the message
is addressed to, what the query holds and where its caret sits.

Like the other GUI harnesses it is not in CI (the daemon is Linux-only and
GitHub's Windows runners cannot host it). Run it when a change touches the pane,
the target picker, or the composer's key routing.

## Reading the GUI smoke

`tools/gui/features/smoke.sh` answers one question —
**does the client boot and run** — in about ten seconds across fourteen checks.
It drives the client through the test hook (`OPENCHIME_DEV_DIR`) and stands up
its own fixture daemon on its own port. It is the GUI check to run before pushing a client change (by hand; the pre-push
hook itself requires only `make test`), on Windows and on the web; it is not a
feature suite and does not aspire to be one.

What it checks is the whole of it, deliberately: the client launches, connects
and authenticates against the fixture; paints the Home shell (the
`sidebar_kind` / `main_is_conversation` / `window_is_covered` predicates);
takes the keyboard; types a message and gets it back from the daemon; opens and
closes the command palette from the composer; and is still answering at the
end. A new view or overlay is added to the predicates in `app.c`; it does not
get a line here.

**Assertions wait on state, never on a clock.** `expect_eventually`, `wait_grep`
and `settle` poll for the state being asserted, so a true assertion returns on
the first poll and only a real failure pays the timeout; the palette is asserted
as a whole closed→open→closed **round trip**, refusing to credit a close whose
open never happened. Tune the patience with `OC_TEST_WAIT_MS` (default 6000).

**Its failures can be believed on sight, which is a property that was built in
rather than hoped for.** It checks the **exit status of every verb**:
`tools/gui/harness/win32/drive.sh` exits non-zero when the client never acked, and a caller that discards that
status turns a dropped command into the *next* assertion failing for an
unrelated reason — a failure that needs interpretation before it can be
trusted.

**Its checks cannot pass by accident either.** The message it sends carries a
per-run unique string, so no assertion can be satisfied by something a previous
run left in the database; and the run ends by making the client answer one more
time, because every earlier check reads a dump file that looks identical whether
the client is alive or died on the last keystroke.

**Keeping it small is the maintenance rule.** A suite of a few hundred
assertions that runs for minutes is skipped, and so
catches nothing — the failure mode of a slow gate is not that it is slow, it is
that it stops being run. Verify a feature by driving it, through
`tools/gui/harness/win32/drive.sh` or a harness scoped to that feature. Do not add it here.

It is not in CI, and in neither suite (docs/TESTING.md): no GUI or browser
check is. Its Win32 run could not be in any case: the daemon is epoll-based so
it is Linux-only, and GitHub's Windows runners cannot host it (no Linux
containers). Run it on both platforms with `tools/gui/run.sh smoke`.

*One caveat when reading a failure message:* the dump's `comp=` field is the
**IME composition length**, not the autocomplete popover — the dump exposes no
popover state, so `comp=0` beside a completion failure means nothing. The
fields worth reading are `error_seq` and `last_error`: without them a failed
intent and an intent that was never sent look identical. Prefer asserting on
`error_seq`, which only ever increments — `last_error` is cleared on
`OC_EV_CONNECTED`/`OC_EV_AUTH_OK`, so a reconnect between the failure and the
check can erase the evidence a wait is polling for.

## Reading the screen-recording harness

`tools/gui/features/screenrec.sh` records with real Windows.Graphics.Capture against its
own fixture daemon (port 9530), with a known picture in the camera box
(`OPENCHIME_DEV_CAPTURE=synthetic-camera`) and known sound
(`OPENCHIME_DEV_AUDIO=synthetic`: the microphone 440 Hz, the computer 660 Hz). It
asserts the screens and windows listed, the box in the chosen corner of the preview
(colour bars at the rectangle the dump's `vmsrc` line and `oc_inset_rect` put it),
the card stepping aside for the recording bar, the bar kept out of the capture and
read and pressed through UI Automation (`tools/gui/harness/win32/uia_recbar.ps1`), Stop ending the
take at the fitted size, the file the daemon stores — found in the fixture's blob
directory — being VP9 and Opus at that size with both tones in it (`ffprobe`,
`ffmpeg`), and a window closed mid-take keeping what came before.

**Keep the Remote Desktop window open while it runs.** A session that is not drawn
captures its screens as black; a window is captured either way. `tools/gui/harness/win32/drive.sh
launch` with `OC_TEST_LOCAL=1` runs the client from a local copy, which a real
camera needs.

## Reading the links harness

`tools/gui/features/links.sh` drives links in the transcript (REQ-220, MARKDOWN.md §4)
against its own fixture daemon (port 9650). It sends a labelled link and a bare
address, then asserts that hovering the label reports the real address, marked
labelled, in the dump's `link hover= labelled=` line; that clicking it opens the
confirmation naming that address (`confirm open= act= url=`) rather than the
browser, and Esc dismisses it; and that hovering the bare address reports it,
unlabelled. Nothing is ever opened: the bare link is only hovered and the
confirmation is always dismissed.

## Reading the reactions harness

`tools/gui/features/reactions.sh` drives the who-reacted pane (REQ-070/071) over
`tools/gui/harness/win32/pair.sh` (port 9580): bob reacts, so alice's pane has somebody else's
reaction to join. It asserts a chip per distinct emoji with its count, marked
when it is yours; that pressing one adds your reaction and pressing it again
takes it back, with the message agreeing; and that each chip is published to
assistive technology saying which it would do. The pane's chips are the
message's own reactions, which every reaction event updates, so the checks do
not depend on whether the daemon's list answered before or after the reaction
was saved. The take-back check requires the
chip to have BEEN yours, or it would pass on a chip that was never pressed. It
then covers the strip of quick reactions on the message under the pointer: six
cells on a fixture that has never saved preferences (the client builds the
default set at start), published to assistive technology, one press to react
and another to take it back -- each press a check of its own, so a strip with
no cells fails rather than skipping the press -- and gone once the pointer
leaves the row. It reads the dump's
`reactors`, `rxnchip` and `hoverreact` lines. The window is sized wide enough
for the context pane the list lives in, or nothing draws, and on Windows pinned
to 96 dpi so a scaled display does not make it narrower in layout. It runs on Win32 and
in the browser (`tools/gui/run.sh --web reactions`). Not in CI, for the
smoke's reason.

## Reading the navigation-history harness

`tools/gui/features/navhistory.sh` (port 9672; on Win32, and in the browser with
`tools/gui/run.sh --web navhistory`) drives Back and Forward through the places
you have been: a channel read scrolled up, a permalink away, Alt+Left back to
the same message, Alt+Right forward, the header's Back button, and a new place
dropping what was forward. It then opens the DMs view's person list and
asserts its header: titled "Direct messages" with Back enabled; the demo
notice, raised by the `banner demo|off` verb without a demo daemon, directly
under it (the banner's top is the header's bottom); its New message opening
the New message pane; and a group DM, once opened, showing its own header
instead. It reads the dump's `hist` line, `header title=... r=... dm_index=...
new=...`, the `banner ... r=` rectangle and `view=`.
## Reading the mentions harness

`tools/gui/features/mentions.sh` drives completion in the composer (REQ-221) over the
pair (port 9730; on Win32, and in the browser with `tools/gui/run.sh --web
mentions`). alice types the start of a name and takes the suggestion by Tab,
by Enter and by a click, and each must leave the whole name and a space in the
box -- the check a completion that dropped its last character (`@alic`) fails.
Down then Enter takes the second row, and Up from the first wraps to the
last; `:grin` and Tab gives the whole emoji -- a surrogate pair in the box's
UTF-16, the case a completion one unit short broke worst -- and a space;
`#gen` and Tab gives `#general `; a
message sent with a completed mention reaches bob's Activity. After a click the
draft is written, as after Tab or Enter; the client also saves what the box
holds on its own, so that check shows the result is kept rather than which
path marked it. It reads the dump's `ac` line and its `acrow` rows (each row's
`repl` and rectangle), `composer ... text=`, `drafthere=` and bob's
`actrows=`. Not in CI, for the smoke's reason.

## Reading the polls harness

`tools/gui/features/polls.sh` drives polls (REQ-225) over the pair (port 9770; on Win32,
and in the browser with `tools/gui/run.sh --web polls`): alice makes and edits
them in the poll composer, and bob votes. The composer is driven by verbs that do
what its controls do -- `pollnew`, `polledit <mid>`, `polltype <q|time|0..9>
<text>`, `polladd`, `pollrm <i>`, `pollcheck <multi|anon> <0|1>`, `polldeadline
none|<YYYY-MM-DD> <time>`, `pollclose`, `pollsubmit`, `pollcancel` -- and every
control is also published to assistive technology (`pollc.*`), so a click works as
well. It asserts that a poll made with an option added, removed and added again,
and a deadline at an arbitrary local minute two days ahead, arrives on both
clients with exactly that deadline; that an edit before anyone votes changes the
question and the options for both; that bob's vote moves the count on alice's side
without marking it hers, and marks it on his; that pressing a row votes and
pressing it again takes the vote back, each row published; that "See who voted"
lists both people with what they picked; that a multiple-choice anonymous poll
says so and offers no list; that once someone has voted the composer locks the
options and settings but still moves the deadline; that "Close poll now" closes
it on both clients, after which no row takes a vote; and that no question, one
option or a deadline already past is refused at the field and posts nothing. It
reads the dump's `pollcomposer`, `pcrow`, `poll`, `pollopt` and `voters` lines.
Not in CI, for the smoke's reason.

## Reading the tables harness

`tools/gui/features/tables.sh` drives tables (REQ-220, MARKDOWN.md §1a) over the pair
(port 9550; on Win32, and in the browser with `tools/gui/run.sh --web tables`).
In the transcript it sends bodies with `sendraw` (which reads `\n` and `\t`) and
asserts on both clients that a table is drawn as a grid with its rows, columns,
header and alignment; that a table with no header, a cell with bold, a link,
emoji and a `<br>` (a taller row), and the text before and after a table all
render; that a wide table scrolls on its own (`tscroll`) while the message does
not; that a link in a cell asks before it opens; that the preview reads "Table:
N rows × M columns"; that a long table stops at its limit with "+N more rows";
that "Copy table" (`copytable`) puts tab-separated text on the clipboard; and
that a malformed table is text. In the composer it builds a table in the table
editor (`tablenew`, `tablecols`, `tableheader`, `tablealign`, `tablecell`,
`tableaddrow`, `tableok`), checks the composer holds one card and no pipes,
sends it between two lines of text, edits the sent message to find the card
again, reopens it (`tableedit`) filled in, changes a cell and saves, and
removes a card and takes the removal back with undo. Pasting puts a sheet's
text, its HTML or a picture of it on the clipboard (`clipset`, `cliphtml`,
`clippicture`) so no run depends on a real spreadsheet, then `paste`, and checks
the chooser offers Table, Image and Text, that Table makes a card keeping the
HTML's links and bold, Text inserts the tabs as typed, Image with no picture on
the clipboard attaches the cells drawn as a PNG (a decodable picture in the
upload tray) and with one attaches that, and Esc pastes nothing. It reads the dump's `table`, `tcell`, `composer`,
`ctable`, `tableeditor`, `tecell`, `pastechoice` and `clipboard` lines. Not in
CI, for the smoke's reason.

## Reading the shortcuts harness

`tools/gui/features/shortcuts.sh` drives the Win32 client's keyboard shortcuts over
`tools/gui/harness/win32/pair.sh` (port 9570) and asserts the EFFECT of each key, not that it
dispatched: two clients, so bob can leave something unread for alice. It
covers Shift+Esc — every conversation read, as the reference client binds it —
and that bare Esc still closes what is open and marks nothing; and Ctrl+<digit>,
which goes to that workspace counting from 1 in the rail's order, with a digit
past the last one doing nothing. The workspace half starts a **second daemon** on
the next port, because a client keys a workspace by its address and signing in
twice to one address is one workspace; and it sets the starting point through the
switcher's own path before each key, so a check cannot begin where it means to
end. It reads the dump's `marks ch … unread=`, `toast[n]` and
`workspaces=/active=` lines. Not in CI, for the smoke's reason.

## Reading the composer harness

`tools/gui/features/composer.sh` drives the Win32 message box against its own fixture
daemon (port 9540) and asserts that the box is always tall enough for its text,
however the text got there: a draft long enough to wrap, restored when the client
starts again; leaving for a conversation with no draft and coming back; a
narrower and a wider window. It reads the dump's `ed` line — `lines=` (what the
text wraps to at the width it is drawn at) against `fit_lines=` (what the box
holds), which must agree up to the box's four-line maximum, past which the text
scrolls.
It also asserts that the formatting row and the action row are built to one
measure — the same button size, left edge and pitch, from the dump's `fmtbar`
and `actrow` lines — and that resting the pointer on each action button shows a
tooltip naming it, as the formatting buttons do (`actrow tip=`). Not in CI, for
the smoke's reason.

## Reading the drafts harness

`tools/gui/features/drafts.sh` deletes drafts in the Win32 client against its own fixture
daemon (port 9560): each Drafts row's Delete, published to assistive technology;
Delete taking the draft at once, asking nothing, its notice offering Undo, and
Undo putting the draft back; a delete gone from the list, the count and the
sidebar, and not written back by the composer, which still holds that
conversation's text; the row's context menu doing the same. Its notices are
dismissed before the next part, since one still up covers rows it reads. It then
checks that every context menu — a message's, a sidebar conversation's, a
member's, a draft's — keeps its row lit while it is open and only while, by
reading the row's pixels from a screenshot with the pointer moved off both the
menu and the row: hover alone could not, since a menu being open is what clears
or freezes it. It reads the dump's `draftn=`, `draftrows` (each row's
conversation, thread and Delete rect), `modal=`, `toast[n]`, `memrow` and
`a11yitem` lines. Not in CI, for the smoke's reason.

## Reading the files harness

`tools/gui/features/files.sh` pages the Win32 files view (REQ-143) against its own
fixture daemon (port 9590) with `OPENCHIME_DEV_FILE_PAGE=2`, so three uploads are
enough to see paging a real daemon would only show past two hundred files. It
asserts that the first page holds its two rows and says more remain; that "Load
more" is published to assistive technology; that pressing it APPENDS the next
page rather than replacing what is shown — three rows, each file once — and that
the button then goes, there being no more. It reads the dump's `files` line:
`n=` (rows held), `more=` (the daemon says more remain), `loading=`, `rows=`
(rows drawn) and `more_btn=` (the button's rect, `0,0,0,0` when it is not there).
The paging underneath is proven without a screen in `test_client_core.c`, which
asks the daemon for pages of two and checks the second continues from the first
rather than repeating it. Not in CI, for the smoke's reason.

## Reading the lightbox harness

`tools/gui/features/lightbox.sh` drives the expanded image view against its own
fixture daemon (port 9710). It uploads three images with a text file between
them, opens one with a click, and pages with Left, Right and the next arrow,
asserting the view's place from the dump's `lightbox=` line: `lightbox=` (the
attachment on screen), `lightbox_pos=<i>/<n>` (its place in the conversation's
media; the text file is never counted), `prev=` and `next=` (whether each arrow
is drawn) and `next_r=` (the next arrow's rect). At the last image Right changes
nothing; Esc closes the view. Run on both platforms, `--web` included.

## Reading the uploads harness

`tools/gui/features/uploads.sh` drives the Win32 message box's upload tray (REQ-140)
against its own fixture daemon (port 9630). On the Windows client it first
drives the real file dialog: `clickform` on "+" opens the system's Open dialog,
whose modal loop holds the client's thread, and the hook still answers because a
thread timer that loop dispatches runs it (`oc_plat_set_modal_tick`; the dump's
`modal=` line says `picker=1` while the dialog is up). The `picker` verb answers
the dialog as a person would: `picker cancel` presses its Cancel, and `picker
pick <path>` puts the path in its file name box and presses OK, so the file
comes back through the same call a person's pick does; with no dialog open
either acks `none`. It asserts that Cancel adds nothing and a picked file comes
back as a chip. Then it attaches files through the `attach` verb, into the tray
without the dialog, and asserts
that they wait as chips, an image with its picture, each chip's button published
to assistive technology, with nothing uploaded; that a chip's × takes it out;
that Send posts what is left and the text as ONE message and the tray empties;
and that a file that cannot be read stops the post, the chip coming back marked
and the text back in the box, with nothing posted — shown by the next message
being the last. Then it pastes, setting the Windows clipboard through PowerShell
the way other programs do and pressing Ctrl+V through the real key path: a copied
file becomes a chip as itself, a copied image a `pasted-image-…png` chip with its
picture that posts under that name, and text still pastes as text with no chip. It reads the dump's `ftray` line (`here=`, `all=`, `posts=`,
`height=`) and one `fchip` line per chip (`pic=`, `state=` -1 waiting for Send,
0 queued, 1 moving; `failed=`, `done=`, and the `x=` button's rect). Its last
step sends a large file so the bars can be seen moving in `uploads_moving`; that
shot asserts nothing, since when a local upload finishes is not something to
test against. The core underneath — one message, its files in order, a thread,
a failure — is proven in `test_client_core.c`, and cancelling in
`itest_slow_blob.c`. Not in CI, for the smoke's reason.

## Reading the members harness

`tools/gui/features/members.sh` scrolls the members pane (REQ-031) against its own
fixture daemon (port 9690) bootstrapped with sixty people — `OC_TEST_USERS`, all of
whom land in `#general` on registration — so the roster is three times what the
pane holds. It asserts that only what fits is drawn and the pane says the rest is
below it; that the wheel reaches **every** member, counted as the distinct people
drawn on the way down (sixty of sixty); that the bottom row is then somebody the
first screen never showed, and answers a click by opening that person's profile;
that the offset stops at both ends rather than running past them; that an
owner's menu on a member, which carries "Reset password and two-step sign-in…",
cuts none of its items (the dump's `menurow … cut=` says whether a label was wider
than its row as drawn); and that switching channel puts the pane back at its own
top. It reads the dump's
`members n= rows= scroll= max=` line and the `memrow uid= r=` lines, and drives
the `wheel` verb in detents of 120. Last, the owner's "Reset password…" on a
member opens the reset to send with all of it shown: the dump's `formlines` line
is the field's own wrapped line count against the lines its box shows, and the
first one visible.

**Counting the people is the check; reading the bottom row is not.** A first
version asserted that the last drawn row was a real member and that the offset
reached its maximum — both of which hold with the scroll offset removed entirely,
because the pane still draws a full first screen and the maximum is computed from
the roster either way. It passed 11/11 against a deliberately broken build. What
distinguishes the two is whether anybody past the first screen is ever drawn.

## Reading the keyboard-menu harness

`tools/gui/features/keymenu.sh` drives the keyboard's route to the context menu
(REQ-269) against its own fixture daemon (port 9610), with the pointer parked off
every row so nothing it asserts can be the mouse's doing. It covers Ctrl+Down and
Ctrl+Up putting the keyboard on a message and moving between them; Shift+F10
opening that message's actions with an item already highlighted; the arrows moving
the highlight and **Enter running the highlighted item**; Esc closing the menu and
Esc again taking the keyboard off the row; "Unread from here" on the middle
message moving the "New" line to it at once (REQ-235), read from the dump's
`unread_from=`; and Shift+F10 with nothing focused opening the conversation's own
menu. It reads the dump's `kbfocus mid= menuhover= menuhovercmd= subhover=
subopen=` line and `menu=`.

**Enter is proven by effect, and the effect is read back.** The harness walks to
"Copy link", presses Enter, then pastes into the composer and requires the
permalink to name the focused message — a menu that merely closed would prove
neither that the highlighted item ran nor that it ran on the right row. The
clipboard is **seeded with something else first**: Windows keeps it across runs,
so a link copied by an earlier run would satisfy that check whether or not this
run copied anything.

**Every verb's ack is checked** (`k`, over `act`, for keys; `gui_snap` for the dump). The script
names a dropped verb where it happens — an unanswered `key` looks
exactly like a key that did nothing, and an unanswered `dump` leaves the previous
file on disk for everything after it to read.

## Reading the calls harness

`tools/gui/features/calls.sh` runs two Win32 clients in a call over `tools/gui/harness/win32/pair.sh`
against its own fixture daemon (port 9620): alice's synthetic microphone at
440 Hz, bob's at 660 Hz (`OPENCHIME_DEV_TONE`). The clients reach the daemon at
the WSL machine's own address rather than 127.0.0.1, because WSL forwards only TCP
there and a call's audio is UDP. It asserts the start and bob's invitation (his
Calls section and a notification), both in the call hearing each other with
every packet decrypting once the keys are in, noise suppression taking a steady
hum out, bob muted and seen muted, push to talk, a per-person volume, only the
starter ending and End for everyone asking first (Esc keeps the call, Enter ends
it), the Calls section gone with no call, the call's banner in #general with Join
and Decline and bob joining from it, the Calls row's own Join, the header phone's
and Mute's tooltip words, the conversation beside the call view (a message sent
from it reaching alice, folded away and back) and its Settings panel, the
missed-call line read from bob's side with Call back starting a call, and screen
sharing — alice shares the
synthetic screen (`OPENCHIME_DEV_CAPTURE=synthetic`), bob's view shows it with
readable, rising frame numbers, full screen and Esc, actual size, the
accessibility names, bob taking over and stopping. It reads the dump's `call` line — `in=
parts= epoch= sent= keepalives= tx_epoch= self= slot=` — its `call.peer` lines —
`packets= lost= level= keyed= muted= undecryptable= volume=` — its `share` line —
`sharer= on= state= bar= view_frames= tex= px_fno= full= actual=`, `px_fno` being
the synthetic screen's frame number read from the decoded pixels — and `calls[n]`
and `callevents`, and its `callui` line — `banner= chat= settings= hdrtip= mutetip=
missed=`. The `call` verb drives it: `call start|join|open|leave|end|decline
[ch]` (`end` is the request End for everyone's confirmation makes; `end-press` presses
the button, which asks), `call banner-join|banner-decline|settings|chat|callback|row-join
<i>`, `call mute|unmute|ptt-down|ptt-up`, `call ns on|off`, `call invite <uid...>`,
`call volume <uid> <percent>`, `call share <device id or name>|pick|stop|full|actual`.
Not in CI, for the smoke's reason.

`tools/gui/features/camera.sh` runs the same kind of pair (port 9676) with the synthetic
camera (`OPENCHIME_DEV_CAPTURE=synthetic`) for cameras in calls (REQ-160): the
call view's `call.camera` and Settings' `call.camera.picker`; alice's own card
showing her camera at once and the roster marking it on; each card showing the
other's camera, its frame numbers rising, at 640x480; the grid, then the speaker
layout with bob large and alice in the strip, and one click back; no card drawn
and no camera taken away from the call view, and both back on returning; bob's
camera off showing his face; and, in the same pair after `devenv
OPENCHIME_DEV_CAPTURE denied`, turning the camera on saying it is blocked in the
video recorder's words. It runs in about 15 seconds on either client, most of
it the pair coming up. It reads the dump's `camera` line — `on= state= err=
shown_err= w= h= fps= kbps= step= frames= cams= chosen= layout= speaker= sub_all=
subs= drawn=` — its `camera.tile` lines, one per card or tile drawn — `uid= cam=
big= fno= rect=`, `fno` the synthetic camera's frame number read from the picture
— and its `camera.peer` lines, the engine's view of each other camera — `on=
subscribed= frames= dropped= w= h=`. The `call` verb drives it: `call camera
on|off|pick|<device id or name>`, `call layout grid|speaker`. Not in CI, for the
smoke's reason.

`tools/gui/features/calls_tcp.sh` is the same pair (port 9670) with UDP that goes
nowhere: the daemon advertises the relay at a port nothing answers on
(`OC_TEST_ADVERTISE_PORT`, which `tools/gui/harness/win32/pair.sh` passes as
`OPENCHIME_MANAGED_AUDIO_ADVERTISE_PORT`). It asserts that each client finds within
seconds that the relay does not answer, moves to the connection transport — the
dump's `call` line says `transport=tcp` — and hears the other's tone
(PROTOCOL.md §5.17, AUDIO.md §4).

`tools/gui/features/groups.sh` drives user groups (REQ-307–309) on a pair of its own
(port 9640, clients reaching the daemon at the WSL address; carol and u1–u9 exist without clients).
- Alice makes `@crew` in Admin → Groups through the tab's own buttons (the `grpbtn` verb presses them, `formnext` fills their forms).
- She adds carol and bob with the people picker (`grppick <query>` types into it through the real key path and presses Enter): `car` finds carol, `#gen` offers nothing and adds nobody, and one Add puts both in.
- In a new private channel, the channel menu's Add someone (`chmenu 6`) opens the members pane's people picker (the window widened so the pane is drawn); `CAR` finds carol, and Enter on an empty query invites her and closes the picker.
- She gives `@crew` to the channel from the channel menu (`chmenu 8`).
- Bob, never invited, is in the channel through the group, and a message naming `@crew` names him.
- His Leave is refused, with the daemon's reason.
- In alice's members pane, bob's Remove (hovered) says why it cannot, and he stays. The picker does not offer bob, who is in already, and Escape closes it. The group row's Remove asks first, and confirmed, takes bob out and leaves carol, who is also in directly. The pane's Add group puts the group back, and the header's Add people invites u1 and u2 in one go.
- Remove someone (`chmenu 7`) opens the roster with no mode and no picker; hovered, u1's row alone offers Remove, and it takes out u1 and nobody else.
- The pane has no rows of buttons: the header carries Add people and GROUPS its +, and resting on each shows its tooltip ("Add people", "Add a group").
- With the picker open, a click in the composer takes the keys: what is typed next is in the composer, not the picker.
- Each of these was shown to fail with its guard reverted: the roster ignoring `info_seq`, the picker offering current members, and the picker keeping the keys after a click elsewhere.
- New message's To field, which shares the picker, still takes a channel, replaces it with people, and refuses a ninth person, saying so. Opening a group's people afterwards gives an empty picker of the group's own, alice can add herself, and New message's recipients and text are still there on return.
- Taken out of the group, bob is out of the channel and told.

The dump's `groups` and `chgroups` lines carry the groups, the open channel's, whether this user is in it through one, and whether its newest message names them. `grppick` carries the picker's owner (`host` 1 a group's, 2 the members pane's), focus, whether the pane is adding (`memmode` 1), chips and matches, and the verb drives whichever of the two is open; `memgrp` the GROUPS subheading's + (`plus`), group-row Removes and the hovered row's Remove; `memppl` the header's Add people, the picker's Add and Cancel, and which icon's tooltip is up (`tip`); `memrow` names each row's person, with its Remove when shown and whether that is dimmed (`via`).

`tools/gui/harness/win32/pair.sh`'s command directories are per port (`ocpair-<port>-a`, `-b`), so two pairs up at once never read each other's commands.

The daemon side is in `make test`:
- `test_dbwriter` covers the rules, membership by reference, `@group`, and the invariant. The invariant is a random sequence of every membership operation checked against a reference model after each step.
- `itest_netloop` runs the same over the wire.
- `test_push` checks that a group mention pushes to its member only.
- `test_client_core` covers the client model and autocomplete.

Each guard was shown to fail with the change it guards reverted.

## Reading the search-box harness

`tools/gui/features/search_boxes.sh` checks the six search boxes (CLIENT.md, "The six
search boxes are one control") on a pair of its own (port 9660; only alice is
driven). At the default text size, the largest, and 200% DPI it opens each box in
turn — the sidebar's find box, search messages, Files, People, Jump to and the
emoji picker — and reads its `searchbox` line from the dump:

- `uifont=1`, and the face is Segoe UI or Segoe UI Variable Text;
- `lf`, the font height, is at least 14 DIP at that DPI (the stock dialog font
  these boxes used to wear is 11 px at every DPI);
- the EDIT is exactly `lh`, the font's line height, lies inside its chrome, and
  is centred in it to a pixel;
- and all six share one font height.

The checks were shown to fail with one box put back on the stock font, and with
the EDIT given a fixed height again.

## Reading the voice harness

`tools/gui/features/voice.sh` drives voice input in the Win32 client end to end against
its own fixture daemon (port 9680), with real recognition: read-aloud renders a
sentence, and `OPENCHIME_DEV_AUDIO=synthetic` with `OPENCHIME_DEV_MIC=<wav>`
makes the client's microphone speak it. It asserts push to talk by the held key,
by UI Automation and by the mouse (words in the composer, nothing sent), free talk
(the words posted, the composer untouched), that leaving the conversation and
opening the video recorder end a session, that a blocked microphone
(`OPENCHIME_DEV_AUDIO=mic-denied`) is reported, and that with `OPENCHIME_STT=0`
there are no controls at all. It reads the dump's `dictate` line — `avail=
offered= on= mode= hold= sent= answered= words= err=` and the button rects — and
its `toast[n]` lines. Not in CI, for the smoke's reason. Run it when a change
touches voice input, the composer's controls or the device layer.

## Reading the cursor harness

The pointer a detached Windows session reports is suppressed, and the harness's
hover is a synthetic mouse event, so neither says what shape the cursor took.
The client does: the test dump's `cursor kind=<arrow|ibeam|hand|none>` line, in
the signed-in dump and the signed-out one alike, is the shape it last set
(`none` before any). `tools/gui/features/cursor.sh` (Windows and the web) sends a
message with a link, then hovers the composer's text, the link and empty space
with `move`, and asserts `ibeam`, `hand` and `arrow` in turn.

## Seeing the whole window

`tools/gui/harness/win32/drive.sh shot <name>` produces one image of the entire
application: `test_shot` renders the scene through the window's own renderer
and reads it back (`gfx_readback`), on every platform the same way. Every field
is drawn now, so what the renderer holds is the whole application, cached
images included; there is no second route to composite and no control a
capture cannot see. (The native children that needed `PrintWindow` and
`WM_PRINTCLIENT` composited on top are gone with the children; that history is
in git.) Only the image *fetch* is suppressed during a capture, so driving the
harness cannot generate transfer traffic.

**A capture is a picture, and a picture is not the only evidence.** The scene
also keeps a **paint ledger** when the harness is driving — every primitive the
frame drew, in order, with its rect, its clip, its colour and a tag
(`gfx_ledger_dump`). It exists because a screenshot answers "what does this look
like" and nothing else: a label clipped to nothing, ink the colour of the
surface behind it, and two strings drawn over each other all produce a picture
that looks right to a comparison and wrong to a person.

Two rules for anything drawing text through it. A text raster carries its colour
in its pixels, so the ledger cannot read one back — the call site declares it
with `gfx_ink`, which also carries the rasterizer's transparent padding so the
recorded rect is the ink and not the bitmap. And a colour that is **content**
rather than a theme choice — an avatar disc derived from a user id, a swatch for
a colour scheme you have not picked — says so with a `content:` tag, or the
palette check reports it as a surface that will not follow the theme.

## `clipped=`: text that does not fit its own box

`chromefit` compares published rectangles, so a label whose box is the right size
and in the right place reads as clean however its glyphs actually landed — the
Notifications card at 240 DPI reported `overlaps=0 outside=0` with its section
titles cut through the middle. `clipped=` closes that: the count of strings whose
layout did not fit the rect it was drawn into, with `clip="…"` naming the first
and `clippx=` the deepest cut in DIPs. The tally belongs to the frame, reset as
each one begins.

**Whole lines, not pixels.** A line box is ascent + descent + leading, pinned per
size token (ARCH-108), and legitimately overhangs a tight row by a DIP or two
with nothing cut off; measuring that reports every second label in the client. A
line that does not fit is not a rounding question.

**Vertically always, horizontally only where nothing trims.** A format that does
not wrap is given DirectWrite's ellipsis, so running out of room ends in a "…" —
legible, deliberate, a question of layout style rather than a defect. A wrapping
format has no trimming to fall back on, so a word longer than its column is cut
mid-glyph and nothing says so. Colour emoji are exempt, as they are for the
ledger's truncation check and for the same reason: an emoji cell clips a few
pixels of trailing advance and looks perfectly right doing it.

**It has failed, which is the only evidence worth having.** Across nine views ×
four DPI settings × the text-size extremes it is silent, and names two real cuts:
the composer's `Message #general` placeholder, gone entirely at 240 DPI, and the
Activity empty state losing its last word at 192 DPI and the largest text size.
Both read `overlaps=0 outside=0`.

## Observing the tray balloon

`tools/gui/features/balloon.sh` answers whether the tray balloon is **drawn**, which
nothing else can: the balloon is the middle of the notification chain — WinRT
toast, else balloon, else the client's own window — and wherever toasts work it
is never reached. The run turns the toast off for its own duration (`wintoast 0`
through the test hook; nothing is saved), sends one notification through the real
chain, and reports two separate facts.

**Accepted** is read from the dump's `notify … by=` field: `2` means
`Shell_NotifyIconW` took the `NIF_INFO` update and the balloon carried the
notification. Anything else fails the run.

**Observed** is a capture of the whole desktop, because the shell draws the
balloon and a capture of our window cannot contain it. A disconnected or locked
session has no composited desktop; the capture is blank, and the run says *not
observed* (exit 2) rather than reporting a balloon that is missing. Focus Assist
hides a balloon the shell has accepted, and the picture is how that is told apart
from a balloon that is drawn. The picture is for a person to look at — what a
balloon looks like varies by Windows version, and on Windows 10 and later it is
rendered as a toast.

**It has been seen.** `by=2` with the desktop captured: the shell drew a dark
notification card with the app icon, `OpenChime`, and the message's source and
text on the two lines under it — the shape a real message produces, since the
run sends one through `notify_deliver` rather than calling the balloon directly.
A disconnected session is what the blank case is for and is not a failure of the
client: the first two runs of this script hit exactly that.

## Reading the jump-to-date harness

`tools/gui/features/jumpdate.sh` sends a few messages into a fresh channel and jumps to
them by date through the test hook's `jumpdate <date>` verb, which runs the
same path as typing that date into the calendar of "Jump to date", in the open
thread when one is. It reads the dump's
`datejump` line: `pending` is the conversation still waiting for the daemon's
answer (0 once it has come), `found` and `exact` are that answer, `jump` is the
message the transcript has still to scroll to (0 once it is on it), and `seps`
counts the date dividers drawn, each of which offers the jump when clicked.
`parent` is the thread a pending jump is in (0 for the main scroll), `thrjump`
the reply the thread pane has still to scroll to, and `cal` whether the
calendar card is open.
Today's date lands on today's first message, exact; a date after the newest
lands on the newest, not exact, with a notice saying so.

## Reading the snippets harness

`tools/gui/features/snippets.sh` posts a snippet through the test hook's
`snippet <title>|<language>|<text>` verb (`\n` for a newline), which is the
path Create snippet takes. It reads the dump's `snipcard` lines, one per card
drawn, with `loaded` 1 once its text has been fetched and `rect` the card to
click, and the `snipview` line: `open`, the snippet it is on, the language
(lexer.h's number) and `spans`, the coloured runs the lexer found. A card that
loads, a click that opens the viewer, Python coloured into several spans, and
Esc closing it make a pass.

## Reading the web pages harness

`tools/gui/features/web_pages.sh` (web only, port 9595, and 9596 for the
callback) drives the daemon's own pages, not the client, from a headless
Chromium through playwright-core, so what a browser itself sends -- its
`Origin` under the pages' referrer policy, its handling of the 303, its cache
-- is what is proven, which no scripted post can. It starts the daemon as
shipped, with no owner yet, and reads the setup token off its log. Its three
browser scripts each print one `ok` or `FAIL` line per check, which the
script counts as its own:

- **The sign-in pages** (`harness/web/pages.mjs`). The first owner signs up
  with the setup token (the form carries the invitation out of sight, says the
  password rule beside the field, refuses a short password saying why, and
  offers a show-password toggle); a wrong password is refused on the page and
  the right one signs in; the password page changes it; the old password is
  refused and the new one signs in. Each sign-in's 303 lands on a loopback
  callback the script serves (as a client's own listener is), and the token
  is read off that address. The last token is then presented on `AUTH` by
  `build/signin present` with the sign-in's PKCE verifier.
- **An invitation link** (REQ-026, `harness/web/invite.mjs`). The owner mints
  an invite anyone may use (`build/demo_client ... invite 0`), and a second
  browser opens `/app/?invite=<hex>` cold: it lands on the sign-up form before
  the client has been fetched, chooses a name and a password, and comes back
  signed in to the web client, which its own dump says, the token taken off
  the address. The tab is closed and the client opened again in a new one,
  still signed in.
- **The client's load** (`harness/web/load.mjs`, WEB.md "Loading"), cold and
  then warm in one browser that trusts the daemon's certificate by its key
  (the SPKI hash, read here with Node: a browser caches nothing from a
  certificate it was told to overlook). The load screen counts the download in
  bytes against its size, shows Downloading, Starting and Opening in that order
  and comes down once the client has drawn; the wasm and the loader arrive
  gzipped; a right-click opens no browser menu; and the second load brings the
  page, the loader and the wasm under 2 KB each.

It needs `build/signin` and `build/demo_client`, which `run.sh` builds for a web
run. Each browser script's output is left in the script's directory
(`/tmp/oc-web-pages/<pages|invite|load>.out` and `.err`).

## Reading the web client harness

`tools/gui/features/web_client.sh` (web only, port 9585) checks what only the
browser build has to show, through the web driver and the client's own verbs.
The client booting, signing in and drawing itself is the smoke's; the screen
recorder is `screenrec`'s.

- **The text layer**: the client's `sttest` verb, the text layer's
  right-to-left order, cluster hit-testing and line breaking through the API the
  views use (`sdltext/st_canvas.c` in the browser), acks `ok n=<checks>` or the
  first failure.
- **A video message** from the synthetic camera (`OPENCHIME_DEV_CAPTURE=synthetic`)
  and Chromium's fake microphone: `vm open` shows the camera (`frame=` on the
  `vm=` line), `vm record` records past three seconds by the recorder's own
  clock (`rec_elapsed`), and `vm stop` takes the file (`take_bytes`) -- the
  desktop's recorder, codecs and muxer, in wasm. The page's console must say the
  audio ran on the worklet (`oc-web: audio worklet`, in the driver's
  `web-a/drive.log`), not the fallback.
- **The input method**: the web driver's `ime update <text>` and `ime commit
  <text>` play a Japanese composition into the page's hidden input for the input
  method (`plat_web.c`), as a desktop input method sends it; the composer's `ed`
  line must show two units composing (`comp=2`), then hold the two committed
  characters (`len=2`, 日本).
- **About**: the `about` verb acks both builds on one line, `OpenChime client
  <build> (web), server <build>, protocol <n>` (the daemon's from
  `SERVER_BUILD`).
