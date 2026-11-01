#!/usr/bin/env bash
# scripts/test/merge/gui_web_smoke.sh
#
# The GUI smoke on the web platform: the scripted walk the Windows smoke takes
# (scripts/gui/smoke.sh), driven in a headless Chrome against the daemon's own
# web client (scripts/gui/run.sh --web).
#
# Needs: node, Chrome, a daemon built with WEB=1.
# Run by: make test-merge (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
need "${NODE:-node}" "Node.js; make passes the pinned emsdk's"
scripts/gui/run.sh --web smoke
ok "the GUI smoke passed on the web"
