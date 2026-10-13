# What a person types here:
#   make              the daemon (openchimed), quickly: no speech, no summaries, no web client
#   make full         everything: the daemon with every feature and its data, and every client
#   make run          a dev daemon on 127.0.0.1:8443 with alice/bob
#   make test         the host test suite
#   make clients      every client below
#   make linux        the Linux client (the TUI today; the GUI when it exists)
#   make windows      the Windows GUI and TUI (mingw-w64)
#   make web          the browser client (emsdk)
#   make web-test     the browser client's checks, in a headless Chromium
#   make clean        everything built, except the downloaded speech models
#   make distclean    everything built, and the remembered configuration
# There is one build: what a box is for -- a deployment, the tests and
# harnesses, the public demo -- is OPENCHIME_MODE when it runs (CONFIG.md).
# With ARCH=aarch64, the daemon cross-built for arm64 into build/aarch64/; with
# RELEASE=1, the host's as the release builds it (zig, glibc 2.34). Neither is
# remembered.
#
# `make full` is remembered: build/.features keeps the switches it used, so a
# plain `make` after it rebuilds the same daemon. One switch at a time works
# too (`make TTS=1`), and is remembered the same way, until `make distclean`.
#
# Every toolchain the clients need -- Emscripten, llvm-mingw for Windows, zig
# for the release's compiler -- is fetched pinned on first use (mk/toolchains.mk),
# so `make full` needs only cc, cmake, python3, git, curl and xz installed.
#
# CI's own: make test-ci, make check-release-cc. Tool binaries are built by
# their path (make build/demo_client, build/bench_load, build/bench_loop,
# build/tuikit-demo, build/tts_pack, build/s3_smoke, build/e2e_client,
# build/sdltext_test.exe, build/gfx_test.exe). The rules live in mk/, one
# file per concern; scripts/check/make_targets.sh keeps this list and the
# targets in step.
PUBLIC   := all full run test clients linux windows web web-test clean distclean
INTERNAL := test-ci check-release-cc ci-only test-ci-plain test-ci-threads FORCE
.PHONY: $(PUBLIC) $(INTERNAL)

CC ?= cc

# -Werror, everywhere, always. A warning is a build failure and not a note in
# the scroll-back, because the cost of a warning is never the one warning: it is
# that the next one arrives into a stream a reader has already trained
# themselves to skip. Fifteen of them accumulated here over the life of the
# tree, every one in code that worked, and together they made the sixteenth
# invisible -- which is the only one that would have mattered.
#
# This is not a lint preference. Nothing else in the build refuses to produce an
# artifact over a warning, so "we do not ship warnings" was a thing said rather
# than a thing enforced, and the feature-PR checklist asked for a clean build it
# could not actually get. A check that is always waived is not a check.
#
# The escape hatch is deliberately per-file and visible: add -Wno-<thing> to one
# target's flags, in this file, with a comment saying why. There is no global
# opt-out and no NOWARN=1, because both would be used once in a hurry and then
# forever.
WARN_CFLAGS := -Wall -Wextra -Werror
CFLAGS ?= -std=c99 -D_GNU_SOURCE -O2
# APPENDED, not folded into the ?= above, and that is the whole point. `CFLAGS ?=`
# is one `make CFLAGS=...` away from being replaced wholesale -- and the release
# workflow does pass variables on the command line (CC, OC_BUILD_ID), so the one
# build that must not carry warnings is exactly the one best placed to drop the
# flag that forbids them. `override` is what makes this hold: a plain `+=` still
# loses to a command-line assignment, which is precisely the case being guarded.
override CFLAGS += $(WARN_CFLAGS)
LDFLAGS ?= -lpthread -lm   # -lm: SQLite FTS5 ranking (bm25) calls log()

# The default goal, stated rather than inherited from whichever rule happens to
# appear first. Without this, adding a rule above `all:` silently makes that rule
# what a bare `make` builds.
.DEFAULT_GOAL := all

# --- Features -----------------------------------------------------------------
# What the daemon is built with. All off by default, so a first `make` is the
# daemon alone in a minute or two: read-aloud (TTS) and voice input (STT) need
# ONNX Runtime built from source (about 20 minutes, once) and model downloads;
# summaries (SUM) need llama.cpp; the web client (WEB) needs Emscripten and a
# wasm build of SDL3 and the codecs. `make full` turns them all on.
#
# The switches are remembered: build/.features holds the values of the last
# run, is read back at the top of the next, and a value given on the command
# line wins over it (make's own precedence). Rewritten only when a value
# changes, so an unchanged configuration rebuilds nothing; `make distclean`
# forgets it.
FEATURES := build/.features
-include $(FEATURES)
TTS ?= 0
STT ?= 0
SUM ?= 0
WEB ?= 0
# The architecture and the release compiler are NOT remembered: `make
# ARCH=aarch64` cross-builds the daemon for arm64 (build only: nothing here can
# run it) with zig into build/aarch64/, and `make RELEASE=1` builds the host's
# with zig against glibc 2.34 as the release does; both are a choice made per
# invocation, and a plain `make` is the host's daemon again (mk/toolchains.mk).
HOST_ARCH := $(shell uname -m)
ARCH    ?= $(HOST_ARCH)
RELEASE ?= 0
FEATURES_NOW := TTS=$(TTS) STT=$(STT) SUM=$(SUM) WEB=$(WEB)
$(FEATURES): FORCE | build
	@printf '%s\n' $(FEATURES_NOW) | cmp -s - $@ || printf '%s\n' $(FEATURES_NOW) > $@

# Build identity (ARCH-20). The candidate build passes the short commit it is
# building (OC_BUILD_ID=74359c0), which is what `--version` and the startup
# log say; a source build leaves it unset and reports "dev". A release number
# is a label assigned to a build id afterwards (docs/RELEASING.md) and is
# never compiled in. The daemon and the TUI take it; the tests link the same
# objects and have no use for it.
OC_BUILD_ID ?=
ifneq ($(OC_BUILD_ID),)
VERSION_DEF := -DOC_BUILD_ID='"$(OC_BUILD_ID)"'
else
VERSION_DEF :=
endif

FORCE:
build:
	mkdir -p build

# The rules, one file per concern. The order matters only in that each file
# uses what the ones before it name.
include mk/toolchains.mk
# The compiler the build/ outputs were made with. Without it `make CC=clang test`
# after a gcc build finds build/tests up to date and runs the gcc binary; with it,
# a different $(CC) rebuilds what it compiles. Rewritten only when $(CC) changes,
# so an unchanged compiler rebuilds nothing. After mk/toolchains.mk, which may
# have chosen the compiler and the build directory.
CC_STAMP := build$(BUILD_SUFFIX)/.cc
$(CC_STAMP): FORCE | build
	@mkdir -p $(dir $@)
	@echo '$(CC)' | cmp -s - $@ || echo '$(CC)' > $@
include mk/sources.mk
include mk/vendor.mk
include mk/daemon.mk
include mk/web.mk
include mk/linux.mk
include mk/windows.mk
include mk/tests.mk

all: $(BIN) $(TTS_DATA) $(STT_DATA)

full:
	@$(MAKE) --no-print-directory TTS=1 STT=1 SUM=1 WEB=1 all clients

# One-shot local dev server: builds the daemon + TUI and runs the daemon on
# 127.0.0.1:8443 with sane paths + bootstrap users (alice/pw, bob/pw). Connect
# from another terminal: build/openchime-tui 127.0.0.1 8443 alice:pw
run: $(BIN) $(TUI_BIN)
	@mkdir -p /tmp/openchime-dev
	@echo "openchime dev daemon -> 127.0.0.1:8443  (users: alice/pw, bob/pw)"
	@echo "connect: build/openchime-tui 127.0.0.1 8443 alice:pw"
	@OPENCHIME_MODE=development OPENCHIME_DATA_DIR=/tmp/openchime-dev OPENCHIME_PORT=8443 OPENCHIME_HEALTH_PORT=8080 \
	 OPENCHIME_DEV_USERS="alice:pw:owner,bob:pw:member" ./$(BIN)

# Every client the tree builds, one per platform: macOS, iOS and Android join
# this list when they exist.
clients: linux windows web

# Everything built, except the speech models under build/kitten and
# build/moonshine: they are downloads, not builds, and cost minutes to fetch
# again. `distclean` removes those too, and the remembered features.
clean:
	rm -f $(BIN)
	find build -mindepth 1 -maxdepth 1 ! -name kitten ! -name moonshine ! -name .features -exec rm -rf {} + 2>/dev/null || true

distclean:
	rm -f $(BIN)
	rm -rf build
