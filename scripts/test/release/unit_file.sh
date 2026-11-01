#!/usr/bin/env bash
# scripts/test/release/unit_file.sh <binary>
#
# The installed openchimed.service parses: its ProtectSystem, DynamicUser and
# RestrictAddressFamilies lines are directives this systemd knows. Nothing at
# install time parses them strictly -- both maintainer scripts start the unit
# only where systemd runs -- so a typo would otherwise ship unchecked.
# systemd-analyze exits 0 even for a directive it does not recognise, so its
# output is read, not its status; the pattern is "Unknown key", which is how
# systemd words it; and only lines about our unit count, since verify reports
# on every unit it loads. Where <binary> (the release's) is for another
# architecture, package.sh installed nothing, and nothing is checked.
#
# Needs: systemd-analyze; the package installed (package.sh).
# Run by: make test-release (PART=packages).
. "$(dirname "$0")/../lib/common.sh"
need systemd-analyze "systemd"
bin=${1:?usage: unit_file.sh <binary>}
if ! runnable "$bin"; then echo "$TEST: $bin is not for this machine ($(uname -m)); nothing installed to check"; exit 0; fi
unit=/lib/systemd/system/openchimed.service
have "$unit" "package.sh installs it"
out=$(systemd-analyze verify "$unit" 2>&1 || true)
printf '%s\n' "$out"
bad=$(printf '%s\n' "$out" | grep openchimed | grep -v "is not executable" \
      | grep -E "Unknown key|Unknown lvalue|Unknown section|Failed to |Invalid " || true)
[ -z "$bad" ] || fail "openchimed.service has a directive this systemd rejects: $bad"
ok "openchimed.service parses clean"
