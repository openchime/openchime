#!/usr/bin/env bash
# scripts/test/full/release_cc.sh
#
# Everything built for Linux -- the daemon with every feature, the client
# app-core, its media, voice and call code, and the tests -- compiled with the
# release's compiler (zig cc against glibc 2.34) under -Werror, stopping short
# of the link. A newer clang refuses things gcc accepts, and the release is the
# wrong place to learn it; this is the tree's second-compiler check too. The
# objects are make's (check-release-cc, mk/tests.mk), one rule each, so they
# compile in parallel under the calling make's job server (MAKE).
#
# Needs: make (zig is fetched pinned on first use).
# Run by: make test-all (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
exec ${MAKE:-make} --no-print-directory check-release-cc
