#!/usr/bin/env bash
# Emit the licence notices that must accompany a distributed daemon binary.
#
# Building from source never triggered this. Distributing binaries does: mbedTLS
# is Apache-2.0 and jsmn is MIT, and both require their notice to travel with the
# binary. The texts are read from the actual trees the build used rather than
# copied into this script, so they cannot drift from what was linked.
#
# Only what the DAEMON links is listed. termbox2 is TUI-only and lucide is
# client artwork: neither is in openchimed, and claiming otherwise would be its
# own kind of wrong. utf8proc and qrcodegen are in both, the daemon normalizing
# passwords with one and drawing the setup page's QR code with the other.
# Read-aloud (ARCH-111), voice input (ARCH-112) and summaries are in the
# release's daemon, and with them the voice model, Opus, CMUdict-derived data
# and the recognizer; for a daemon built without one, run this with
# TTS=0, STT=0 or SUM=0. The export of a video message (EXPORT, ARCH-135) brings
# libavc, LAME, libvpx and Opus; for a daemon built without it, EXPORT=0.
#
# Litestream (Apache-2.0) is not here for the same reason: it is a separate
# program, in the container image only, never linked into openchimed nor in
# the tarball this file describes. Its own LICENSE travels beside it in the
# image (packaging/image-context.sh, /usr/share/doc/litestream/LICENSE).
#
#   [TTS=0] [STT=0] [SUM=0] [EXPORT=0] licenses.sh <mbedtls-dir>
#
# LICENSES_LIST=1 prints the path of each text it would include, one per line,
# and nothing else: scripts/test/fast/notices.sh compares that list with what the
# daemon's build links, through these same conditionals rather than a copy.
set -euo pipefail

MBEDTLS_DIR="${1:?usage: licenses.sh <mbedtls-dir>}"
TTS="${TTS:-1}"
STT="${STT:-1}"
SUM="${SUM:-1}"
EXPORT="${EXPORT:-1}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

LIST="${LICENSES_LIST:-0}"

emit() {
  local name="$1" path="$2"
  if [ "$LIST" = 1 ]; then echo "$path"; return; fi
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

# The plain-text blocks, which a list has no use for.
header() { if [ "$LIST" = 1 ]; then cat >/dev/null; else cat; fi; }

header <<'HEADER'
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

The clients (client/, shared/ and tuikit/ in that repository) are covered by the
same licence; see LICENSING.md there.

THIRD-PARTY COMPONENTS
======================

openchimed links the components below. Each is used under the licence reproduced
in full here, as those licences require.

  Mbed TLS 3.6.7   Apache-2.0   TLS transport, TOFU certificate handling,
                                SHA-256, ES256 verification
  Argon2           CC0-1.0 or   password hashing (the reference
    20190702       Apache-2.0   implementation)
  utf8proc         MIT          Unicode normalization of passwords
  SecLists         MIT          the common passwords refused, embedded
                                as data
  jsmn             MIT          JSON tokenizer (OIDC and webhook payloads)
  picohttpparser   MIT          HTTP request parser (webhooks, /healthz)
  QR Code          MIT          the QR code on the second-step setup page
    generator 1.8.0
  Mozilla CA roots MPL-2.0      the root certificates outbound HTTPS verifies
                                against, embedded as data; the source form is
                                third_party/ca-roots in the repository above
  SQLite 3.53.4    Public       the message database
                   domain
HEADER

if [ "$TTS" = 1 ]; then
  header <<'HEADER'

and, for read-aloud (built into the binary, its data in tts/):

  The voice model      Apache-2.0   read-aloud's speech, shipped as data in tts/
  Opus 1.6.1           BSD-3-Clause encoding read-aloud's audio
  CMUdict              BSD-style    source of the pronunciation data
HEADER
fi
if [ "$STT" = 1 ]; then
  header <<'HEADER'

and, for voice input (built into the binary, its data in stt/):

  The recognizer       MIT          voice input's speech recognition model and
                                    tokenizer, shipped as data in stt/
HEADER
fi

if [ "$EXPORT" = 1 ]; then
  header <<'HEADER'

and, for the export of a video message (built into the binary; used only when
the operator turns OPENCHIME_H264_EXPORT on, which needs their own H.264 patent
licence):

  libavc 1.10.0        Apache-2.0   encoding H.264
  LAME 3.100           LGPL-2.0-or- encoding MP3; its source is
                       later        third_party/lame in the repository above
  libvpx 1.17.0        BSD-3-Clause decoding the VP9 a video message holds
  Opus 1.6.1           BSD-3-Clause decoding its audio
HEADER
fi

if [ "$SUM" = 1 ]; then
  header <<'HEADER'

and, for summaries: no third-party code. The model (Qwen3.5 2B, Apache-2.0)
  is not in the package: it is fetched from Qwen and converted on first use,
  and its licence is kept beside it.
HEADER
fi

# The daemon's own licence ships with the daemon. It is not a third-party notice
# -- it is the offer of terms that makes redistributing this binary lawful, and
# the AGPL requires it to travel with the work.
emit "openchimed -- GNU Affero General Public License v3.0 or later" "${root}/LICENSE"

emit "Mbed TLS 3.6.7 -- Apache License 2.0" "${MBEDTLS_DIR}/LICENSE"
emit "Argon2 (reference implementation) -- CC0 1.0 or Apache License 2.0" "${root}/third_party/argon2/LICENSE"
emit "utf8proc -- MIT License, with the Unicode data licence" "${root}/third_party/utf8proc/LICENSE.md"
emit "SecLists (the common-password list) -- MIT License" "${root}/third_party/pwlist/LICENSE"
emit "jsmn -- MIT License" "${root}/third_party/jsmn/LICENSE"
emit "picohttpparser -- MIT License" "${root}/third_party/picohttpparser/LICENSE"
emit "QR Code generator 1.8.0 -- MIT License" "${root}/third_party/qrcodegen/LICENSE"
emit "Mozilla CA roots -- Mozilla Public License 2.0" "${root}/third_party/ca-roots/LICENSE"
emit "SQLite 3.53.4 -- Public Domain" "${root}/third_party/sqlite-3.53.4/LICENSE"

if [ "$TTS" = 1 ]; then
  emit "The voice model -- Apache License 2.0" "${root}/ttskit/data/VOICE-LICENSE"
  emit "Opus 1.6.1 -- BSD 3-Clause License" "${root}/third_party/opus/COPYING"
  emit "CMU Pronouncing Dictionary -- BSD-style licence" "${root}/ttskit/data/CMUDICT-LICENSE"
fi
if [ "$STT" = 1 ]; then
  emit "The recognizer -- MIT License" "${root}/sttkit/data/RECOGNIZER-LICENSE"
fi
if [ "$EXPORT" = 1 ]; then
  emit "libavc 1.10.0 -- Apache License 2.0" "${root}/third_party/libavc/LICENSE"
  emit "libavc 1.10.0 -- NOTICE" "${root}/third_party/libavc/NOTICE"
  emit "LAME 3.100 -- GNU Library General Public License v2 or later" "${root}/third_party/lame/COPYING"
  emit "libvpx 1.17.0 -- BSD 3-Clause License" "${root}/third_party/libvpx/LICENSE"
  emit "libvpx 1.17.0 -- Additional IP Rights Grant (Patents)" "${root}/third_party/libvpx/PATENTS"
  # Opus once, whichever feature brought it.
  [ "$TTS" = 1 ] || emit "Opus 1.6.1 -- BSD 3-Clause License" "${root}/third_party/opus/COPYING"
fi
