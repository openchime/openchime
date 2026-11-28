#!/usr/bin/env bash
# scripts/test/ui/harness.sh
#
# The UI harness (docs/details/TESTING.md, "The UI harness"): build/tests
# running the UI suites, which drive the Windows client (build/openchime.exe)
# against a daemon in the test binary's process, headless unless
# OC_UI_VISIBLE=1, and compare each step's tree and frame with tests/ui/ref/
# (OC_UI_UPDATE=1 writes them instead). In neither suite: it needs a machine
# that runs Windows programs.
#
# Needs: build/tests, build/openchime.exe.
# Run by: make test-ui.
. "$(dirname "$0")/../lib/common.sh"
have build/tests "make test-ui builds it"
have build/openchime.exe "make test-ui builds it"
export OC_TEST_ONLY=run_ui_harness_tests OC_TEST_JOBS=1
exec ./build/tests
