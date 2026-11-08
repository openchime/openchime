#!/usr/bin/env bash
# scripts/test/fast/source.sh
#
# The checks a C test cannot make, because they read the tree's text, run
# before the suites: no two message types share an opcode (opcodes.sh); no
# file cites an issue by number (scripts/policy/refs.sh); the Makefile's typed
# targets are the ones its header names (make_targets.sh); the configuration
# registry is the one source of every OPENCHIME_* name, CONFIG.md's reference
# and the packaged environment file (config.sh); nothing that ships is a box
# in development mode (release_lint.sh); the tests wait on outcomes and
# compare markers, not words (tests_lint.sh); every third-party component the
# daemon links has its licence notice (notices.sh); the web harness keeps one
# driver per client (web_driver.sh); a release lists, closes and sends the
# issues it shipped by the same rules (shipped_issues.sh); and no retired
# deployment-model word is back in the tree (words.sh).
#
# Needs: the daemon built (config.sh asks it for its registry).
# Run by: make test; make test-all (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
scripts/test/fast/opcodes.sh shared/protocol.h
scripts/policy/refs.sh
for check in make_targets config release_lint tests_lint notices web_driver shipped_issues workflow_rules words; do
    scripts/test/fast/$check.sh
done
