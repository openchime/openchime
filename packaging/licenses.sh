#!/usr/bin/env bash
# Emit the licence notices that must accompany a distributed daemon binary.
#
# Building from source never triggered this. Distributing binaries does: mbedTLS
# is Apache-2.0 and jsmn is MIT, and both require their notice to travel with the
# binary. The texts are read from the actual trees the build used rather than
# copied into this script, so they cannot drift from what was linked.
#
# Only what the DAEMON links is listed. termbox2 and utf8proc are TUI-only and
# lucide is client artwork; none is in openchimed, and claiming otherwise would
# be its own kind of wrong.
#
#   licenses.sh <mbedtls-dir>
set -euo pipefail

MBEDTLS_DIR="${1:?usage: licenses.sh <mbedtls-dir>}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

emit() {
  local name="$1" path="$2"
  if [ ! -f "$path" ]; then
    echo "licenses.sh: missing licence text for ${name}: ${path}" >&2
    echo "licenses.sh: refusing to ship a binary without it" >&2
    exit 1
  fi
  echo
  echo "================================================================================"
  echo "${name}"
  echo "================================================================================"
  echo
  cat "$path"
}

cat <<'HEADER'
OpenChime daemon (openchimed)
=============================

Copyright (c) Danny Heskett.

openchimed is free software: you may redistribute it and modify it under the
terms of the GNU Affero General Public License, version 3 or (at your option)
any later version, as published by the Free Software Foundation. It is
distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY --
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE. See the licence for the full terms.

The complete corresponding source is at https://github.com/openchime/openchime
and the licence is reproduced below. Note section 13: if you run a modified
openchimed as a network service, its users are entitled to that modified source.

The client (client/, shared/ and tuikit/ in that repository) is MIT-licensed and
is not covered by the AGPL; see LICENSING.md there.

THIRD-PARTY COMPONENTS
======================

openchimed links the components below. Each is used under the licence reproduced
in full here, as those licences require.

  Mbed TLS 3.6.2   Apache-2.0   TLS transport, TOFU certificate handling,
                                SHA-256/PBKDF2, ES256 verification
  jsmn             MIT          JSON tokenizer (OIDC and webhook payloads)
HEADER

# The daemon's own licence ships with the daemon. It is not a third-party notice
# -- it is the offer of terms that makes redistributing this binary lawful, and
# the AGPL requires it to travel with the work.
emit "openchimed -- GNU Affero General Public License v3.0 or later" "${root}/LICENSE"

emit "Mbed TLS 3.6.2 -- Apache License 2.0" "${MBEDTLS_DIR}/LICENSE"
emit "jsmn -- MIT License" "${root}/third_party/jsmn/LICENSE"
