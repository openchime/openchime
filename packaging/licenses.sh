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

Copyright (c) Danny Heskett. All rights reserved.

This package is distributed as a binary. No licence is granted to the OpenChime
source code, which is not public.

THIRD-PARTY COMPONENTS
======================

openchimed links the components below. Each is used under the licence reproduced
in full here, as those licences require.

  Mbed TLS 3.6.2   Apache-2.0   TLS transport, TOFU certificate handling,
                                SHA-256/PBKDF2, ES256 verification
  jsmn             MIT          JSON tokenizer (OIDC and webhook payloads)
HEADER

emit "Mbed TLS 3.6.2 -- Apache License 2.0" "${MBEDTLS_DIR}/LICENSE"
emit "jsmn -- MIT License" "${root}/third_party/jsmn/LICENSE"
