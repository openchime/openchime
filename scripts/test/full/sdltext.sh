#!/usr/bin/env bash
# scripts/test/full/sdltext.sh
#
# sdltext's test program on the Linux GUI's text backend, FreeType, HarfBuzz
# and fontconfig (sdltext/st_test.c, docs/details/TESTING.md, "The Linux GUI's
# test programs"): layout, range styling, the hit tests, inline boxes, line
# metrics, right-to-left text, fallback for a code point the family lacks, and
# the pixels a memory sink receives. Its exit code is its failure count. The
# bounds that depend on the font are DejaVu Sans's, so fontconfig must find it:
# without it the program fails, rather than measuring another font against
# them.
#
# Needs: build/sdltext_test; fc-list (fontconfig) and the DejaVu fonts
# (fonts-dejavu-core).
# Run by: make test-all (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
have build/sdltext_test "make build-all PART=plain builds it"
need fc-list "fontconfig"
[ -n "$(fc-list 'DejaVu Sans' family)" ] || fail "fontconfig finds no DejaVu Sans (fonts-dejavu-core)"
rc=0
out=$(build/sdltext_test 2>&1) || rc=$?
echo "$out"
[ "$rc" = 0 ] || fail "build/sdltext_test failed: $(echo "$out" | tail -n 1)"
ok "$(echo "$out" | tail -n 1 | sed 's/^sdltext_test (\([^)]*\)): /the \1 backend: /')"
