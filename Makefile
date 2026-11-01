# What a person types here:
#   make              the daemon (openchimed), quickly: no speech, no summaries, no web client
#   make full         everything: the daemon with every feature and its data, and every client
#   make run          a dev daemon on port 8443, every interface, alice/bob
#   make test         the fast tests, before every push (local)
#   make test-merge   everything a pull request must pass (CI only; PART=plain|threads)
#   make test-release the checks on what the release ships (CI only; PART=packages|image|windows)
#   make clients      every client below
#   make linux        the Linux client (the TUI today; the GUI when it exists)
#   make windows      the Windows GUI and TUI (mingw-w64)
#   make web          the browser client (emsdk)
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
# so `make full` needs only cc, cmake, python3, git, curl, xz, bzip2 and nasm
# installed.
#
# Each test is a script in scripts/test/ that one of these targets calls; the
# workflows call only the targets. Tool binaries are built by
# their path (make build/demo_client, build/bench_load, build/bench_loop,
# build/tuikit-demo, build/tts_pack, build/s3_smoke, build/e2e_client,
# build/signin, build/fuzz/frame, build/fuzz/framebuf, build/fuzz/seeds). The rules live in
# mk/, one file per concern; scripts/test/local/make_targets.sh keeps the typed
# targets (the list above and PUBLIC/INTERNAL) in step, not this tool list.
PUBLIC   := all full run test test-merge test-release clients linux windows web clean distclean
INTERNAL := test-merge-plain test-merge-threads test-release-packages test-release-image test-release-windows check-release-cc FORCE
.PHONY: $(PUBLIC) $(INTERNAL)

CC ?= cc

# -Werror, everywhere, always: a warning is a build failure, not a note in the
# scroll-back. The escape hatch is per-file and visible: -Wno-<thing> on one
# target, here, with a comment saying why; there is no global opt-out.
WARN_CFLAGS := -Wall -Wextra -Werror
CFLAGS ?= -std=c99 -D_GNU_SOURCE -O2
# APPENDED, not folded into the ?= above, and that is the whole point. `CFLAGS ?=`
# is one `make CFLAGS=...` away from being replaced wholesale -- and the staging
# build does pass variables on the command line (RELEASE, ARCH, OC_BUILD_ID), so the one
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

# Build identity (ARCH-20). The staging build passes the short commit it is
# building (OC_BUILD_ID=a30809b), which is what `--version` and the startup
# log say; a source build leaves it unset and reports "dev". A release number
# is a label assigned to a build id afterwards (docs/UPGRADING.md) and is
# never compiled in. The daemon, the TUI and both GUI clients take it, through
# shared/build_id.h; the tests link the same objects and have no use for it.
OC_BUILD_ID ?=
ifneq ($(OC_BUILD_ID),)
VERSION_DEF := -DOC_BUILD_ID='"$(OC_BUILD_ID)"'
else
VERSION_DEF :=
endif
# The build id as a file, rewritten only when it changes, so every binary that
# compiles it in is rebuilt when the id is not the one it was compiled with
# (and the web client's one object that says it, About OpenChime).
BUILD_ID_STAMP := build/.build_id
$(BUILD_ID_STAMP): FORCE | build
	@printf '%s\n' '$(OC_BUILD_ID)' | cmp -s - $@ || printf '%s\n' '$(OC_BUILD_ID)' > $@

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
# port 8443 (health 8080) with sane paths + bootstrap users (alice/pw, bob/pw).
# The daemon binds every interface (0.0.0.0 and [::]), not loopback: anyone
# who can reach this machine can sign in with those passwords. Run it only on
# a trusted network or behind a firewall. Connect from another terminal:
# build/openchime-tui 127.0.0.1 8443, then sign in as alice/pw
#
# LEAN=1 runs the same daemon under the lean profile's ceiling (ARCH-4,
# REQ-210): 256 MB with no swap and one CPU, in a transient systemd user scope,
# so running out of memory ends in the kill a small box gives rather than
# swapping. An approximation of a small machine, not a copy of one. Where no
# user scope can be made it stops and says so; it never runs unconstrained.
LEAN ?= 0
LEAN_SCOPE := systemd-run --user --scope --quiet -p MemoryMax=256M -p MemorySwapMax=0 -p CPUQuota=100%
run: $(BIN) $(TUI_BIN)
	@mkdir -p /tmp/openchime-dev
	@if [ "$(LEAN)" = 1 ] && ! $(LEAN_SCOPE) true >/dev/null 2>&1; then \
	   echo "make run LEAN=1: cannot create a systemd user scope here (no systemd, no user session, or cgroup v2 delegation of the memory and cpu controllers missing)." >&2; \
	   echo "The daemon was not started: it would have run without the limits. Run it on a host with a systemd user session, or in a 256 MB, one-CPU VM or container; or run make run without LEAN." >&2; \
	   exit 1; \
	 fi
	@echo "openchime dev daemon -> port 8443 on EVERY interface, not just loopback  (users: alice/pw, bob/pw)"
	@[ "$(LEAN)" != 1 ] || echo "lean profile: MemoryMax=256M, MemorySwapMax=0, CPUQuota=100% (one CPU), in a systemd user scope"
	@echo "connect: build/openchime-tui 127.0.0.1 8443  (sign in as alice/pw)"
	@OPENCHIME_MODE=development OPENCHIME_DATA_DIR=/tmp/openchime-dev OPENCHIME_PORT=8443 OPENCHIME_HEALTH_PORT=8080 \
	 OPENCHIME_DEV_USERS="alice:pw:owner,bob:pw:member" $(if $(filter 1,$(LEAN)),$(LEAN_SCOPE) -- )./$(BIN)

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
