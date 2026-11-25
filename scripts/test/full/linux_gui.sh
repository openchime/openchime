#!/usr/bin/env bash
# scripts/test/full/linux_gui.sh
#
# The Linux GUI's test program (tests/linux_gui_test.c, docs/details/TESTING.md,
# "The Linux GUI's test programs"): the platform, image decoding, desktop
# notifications, the AT-SPI provider, capture through the portals and the
# desktop's settings, each against stand-in D-Bus services the program serves
# itself, and the capture stream from a real PipeWire. They live on a session
# bus of the run's own, which dbus-run-session starts and stops around it, and
# the program starts and stops the PipeWire and WirePlumber itself, under
# directories of its own, so nothing on the machine's own bus or PipeWire is
# touched and nothing is left running. Its exit code is its failure count; a
# check it skips is a failure where CI is set (tests/check.h).
#
# Needs: build/linux_gui_test; dbus-run-session (dbus); pipewire; wireplumber.
# Run by: make test-all (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
have build/linux_gui_test "make build-all PART=plain builds it"
need dbus-run-session "dbus"
need pipewire "pipewire"
need wireplumber "wireplumber"
rc=0
out=$(dbus-run-session -- build/linux_gui_test 2>&1) || rc=$?
echo "$out"
[ "$rc" = 0 ] || fail "build/linux_gui_test failed: $(echo "$out" | tail -n 1)"
ok "$(echo "$out" | tail -n 1 | sed 's/^linux_gui_test: //'), on a private session bus"
