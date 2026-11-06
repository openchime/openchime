#!/usr/bin/env bash
# scripts/test/full/gui_web_smoke.sh
#
# The GUI smoke on the web platform: the scripted walk the Windows smoke takes,
# driven in a headless Chrome against the daemon's own web client. The walk is
# the GUI harness's own smoke script, tools/gui/smoke.sh, run through
# tools/gui/run.sh --web: of the manual harnesses under tools/, this is the one
# a suite runs, and the one a change to them must keep passing.
#
# Needs: node, Chrome, a daemon built with WEB=1.
# Run by: make test-all (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
need "${NODE:-node}" "Node.js; make passes the pinned emsdk's"
tools/gui/run.sh --web smoke
ok "the GUI smoke passed on the web"
