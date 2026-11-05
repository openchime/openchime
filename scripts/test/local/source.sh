#!/usr/bin/env bash
# scripts/test/local/source.sh
#
# The checks a C test cannot make, because they read the tree's text, run
# before the suites: no two message types share an opcode (opcodes.sh); no
# file cites an issue by number (refs.sh); the Makefile's typed targets are
# the ones its header names (make_targets.sh); the configuration registry is
# the one source of every OPENCHIME_* name, CONFIG.md's reference and the
# packaged environment file (config.sh); nothing that ships is a box in
# development mode (release_lint.sh); the tests wait on outcomes and compare
# markers, not words (tests_lint.sh); every third-party component the
# daemon links has its licence notice (notices.sh); the web harness keeps
# one driver per client (web_driver.sh); and a release lists, closes and
# sends the issues it shipped by the same rules (shipped_issues.sh).
#
# Needs: the daemon built (config.sh asks it for its registry).
# Run by: make test.
. "$(dirname "$0")/../lib/common.sh"
scripts/test/local/opcodes.sh shared/protocol.h
for check in refs make_targets config release_lint tests_lint notices web_driver shipped_issues; do
    scripts/test/local/$check.sh
done
