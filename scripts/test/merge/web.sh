#!/usr/bin/env bash
# scripts/test/merge/web.sh
#
# The browser client's checks (docs/TESTING.md §3.4): the gfx pixel test
# under Node, then the client signing in, drawing, recording and composing in
# a headless Chrome (scripts/web/test.sh), then the sign-in pages from a real
# browser (scripts/web/pages.sh), against a daemon built with the client in it.
#
# Needs: node (NODE, the pinned emsdk's), Chrome (CHROME, else google-chrome),
# build/web/gfx-test.js, a daemon built with WEB=1.
# Run by: make test-merge (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
need "${NODE:-node}" "Node.js; make passes the pinned emsdk's"
have build/web/gfx-test.js "make test-merge builds it"
"${NODE:-node}" build/web/gfx-test.js
scripts/web/test.sh
scripts/web/pages.sh
ok "the web client passed in a headless Chrome"
