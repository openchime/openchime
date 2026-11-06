# What a person types here:
#   make build        the fast build, also a bare `make`: the daemon alone, and the Windows clients
#   make test         the fast suite, before every push (local only)
#   make build-all    the full build: everything a release ships (CI only)
#   make test-all     the full suite, once per release-candidate (CI only)
#   make run          a dev daemon on 127.0.0.1:8443, alice/bob, and the Linux TUI
#   make clean        everything built, except the downloaded speech models
#   make distclean    everything built, and the remembered configuration
# The two suites, what each runs and where, are docs/TESTING.md; each test is
# a script in scripts/test/fast/ or scripts/test/full/ that one of these
# targets calls, and the workflows call only the targets.
#
# There is one build: what a box is for -- a deployment, the tests and
# harnesses, the public demo -- is OPENCHIME_MODE when it runs (CONFIG.md).
#
# `make build` and `make test` build exactly one configuration, whatever was
# asked before, and run every compile at once (-j, one job per CPU). Anything
# else is built by its path, and takes the switches below: `make openchimed
# TTS=1` (the daemon with read-aloud), `make build/openchime-tui` (the Linux
# TUI), `make build/web/openchime.js` (the web client). The daemon's switches
# are remembered: build/.features keeps the ones used, so a later `make
# openchimed` or `make run` builds the same daemon, until `make distclean`.
# `make build` and `make test` neither read it nor change it. With
# ARCH=aarch64, the daemon cross-built for arm64 into build/aarch64/; with
# RELEASE=1, the host's as the release builds it (zig, glibc 2.34). Neither is
# remembered.
#
# Every toolchain the clients need -- Emscripten, llvm-mingw for Windows, zig
# for the release's compiler -- is fetched pinned on first use (mk/toolchains.mk),
# so the full build needs only cc, cmake, python3, git, curl, xz, bzip2 and nasm
# installed.
#
# Tool binaries are built by their path (make build/demo_client,
# build/bench_load, build/tts_pack, build/s3_smoke, build/e2e_client,
# build/signin, build/fuzz/<harness>, build/fuzz/seeds). The rules live in
# mk/, one file per concern; scripts/test/fast/make_targets.sh keeps the typed
# targets (the list above and PUBLIC/INTERNAL) in step, not this tool list.
PUBLIC   := build test build-all test-all run clean distclean
INTERNAL := daemon fast-build fast-test build-dir linux windows web check-release-cc FORCE build-all-plain build-all-threads build-all-packages build-all-image build-all-windows test-all-plain test-all-threads test-all-packages test-all-image test-all-windows
.PHONY: $(PUBLIC) $(INTERNAL)

CC ?= cc

# -Werror, everywhere, always: a warning is a build failure, not a note in the
# scroll-back. The escape hatch is per-file and visible: -Wno-<thing> on one
# target, here, with a comment saying why; there is no global opt-out.
WARN_CFLAGS := -Wall -Wextra -Werror
CFLAGS ?= -std=c99 -D_GNU_SOURCE -O2
# APPENDED, not folded into the ?= above, and that is the whole point. `CFLAGS ?=`
# is one `make CFLAGS=...` away from being replaced wholesale -- and the full
# build does pass variables on the command line (RELEASE, ARCH, OC_BUILD_ID), so the one
# build that must not carry warnings is exactly the one best placed to drop the
# flag that forbids them. `override` is what makes this hold: a plain `+=` still
# loses to a command-line assignment, which is precisely the case being guarded.
override CFLAGS += $(WARN_CFLAGS)
LDFLAGS ?= -lpthread -lm   # -lm: SQLite FTS5 ranking (bm25) calls log()

# The default goal, stated rather than inherited from whichever rule happens to
# appear first. Without this, adding a rule above `build:` silently makes that
# rule what a bare `make` builds.
.DEFAULT_GOAL := build

# --- Features -----------------------------------------------------------------
# What the daemon is built with. All off by default, and always off in `make
# build`: read-aloud (TTS) and voice input (STT) need ONNX Runtime built from
# source (about 20 minutes, once) and model downloads; summaries (SUM) need
# llama.cpp; the web client (WEB) needs Emscripten and a wasm build of SDL3 and
# the codecs. The full build turns them all on.
#
# The switches are remembered: build/.features holds the values of the last
# run, is read back at the top of the next, and a value given on the command
# line wins over it (make's own precedence). Rewritten only when a value
# changes; `make distclean` forgets it. REMEMBER=0 neither reads nor writes
# it, and is what the four suite targets (build, test, build-all, test-all)
# and the makes they start run with: `make build` and `make test` leave it as
# it was, and nothing in CI depends on it.
#
# What the objects were compiled with is a different file, FEATURES_STAMP,
# beside them: rewritten whenever the switches differ from the last build's,
# remembered or not, so a build with other switches recompiles what they
# change, and nothing compiled one way is linked into a build asked for
# another.
REMEMBER ?= $(if $(filter build test fast-% build-all% test-all%,$(or $(MAKECMDGOALS),build)),0,1)
FEATURES := build/.features
ifeq ($(REMEMBER),1)
-include $(FEATURES)
endif
TTS ?= 0
STT ?= 0
SUM ?= 0
WEB ?= 0
# The architecture and the release compiler are NOT remembered: `make
# ARCH=aarch64` cross-builds the daemon for arm64 (build only: nothing here can
# run it) with zig into build/aarch64/, and `make RELEASE=1` builds the host's
# with zig against glibc 2.34 as the release does; both are a choice made per
# invocation, and a plain `make openchimed` is the host's daemon again
# (mk/toolchains.mk).
HOST_ARCH := $(shell uname -m)
ARCH    ?= $(HOST_ARCH)
RELEASE ?= 0
FEATURES_NOW := TTS=$(TTS) STT=$(STT) SUM=$(SUM) WEB=$(WEB)
ifeq ($(REMEMBER),1)
$(FEATURES): FORCE | build-dir
	@printf '%s\n' $(FEATURES_NOW) | cmp -s - $@ || printf '%s\n' $(FEATURES_NOW) > $@
endif

# Build identity (ARCH-20). The full build passes the short commit it is
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
$(BUILD_ID_STAMP): FORCE | build-dir
	@printf '%s\n' '$(OC_BUILD_ID)' | cmp -s - $@ || printf '%s\n' '$(OC_BUILD_ID)' > $@

FORCE:
# The directory, as an order-only prerequisite: `build` is the fast build.
build-dir:
	@mkdir -p build

# The rules, one file per concern. The order matters only in that each file
# uses what the ones before it name.
include mk/toolchains.mk
# The compiler the build/ outputs were made with. Without it `make CC=clang test`
# after a gcc build finds build/tests up to date and runs the gcc binary; with it,
# a different $(CC) rebuilds what it compiles. Rewritten only when $(CC) changes,
# so an unchanged compiler rebuilds nothing. After mk/toolchains.mk, which may
# have chosen the compiler and the build directory.
CC_STAMP := build$(BUILD_SUFFIX)/.cc
$(CC_STAMP): FORCE | build-dir
	@mkdir -p $(dir $@)
	@echo '$(CC)' | cmp -s - $@ || echo '$(CC)' > $@
include mk/sources.mk
# The switches the objects in $(OBJ_DIR) were compiled with (Features, above):
# set by mk/sources.mk's OBJ_DIR, so a build of its own (the host daemon a
# cross build writes its manifests with) keeps its own.
FEATURES_STAMP ?= $(OBJ_DIR)/.features
$(FEATURES_STAMP): FORCE | build-dir
	@mkdir -p $(dir $@)
	@printf '%s\n' $(FEATURES_NOW) | cmp -s - $@ || printf '%s\n' $(FEATURES_NOW) > $@
include mk/vendor.mk
include mk/daemon.mk
include mk/web.mk
include mk/linux.mk
include mk/windows.mk
include mk/tests.mk
include mk/full.mk

# --- The fast build and the fast suite (docs/TESTING.md) -----------------------
# One configuration, whatever was asked or remembered: the daemon with no
# speech, summaries or web client, and the Windows clients; the Linux TUI is
# the full build's. A make of its own, with the switches on its command line
# and REMEMBER=0, so build/.features is neither read nor rewritten, and with
# one job per CPU unless the caller already chose (-j).
FAST_MAKE = $(MAKE) --no-print-directory REMEMBER=0 TTS=0 STT=0 SUM=0 WEB=0 \
            $(if $(filter -j%,$(MAKEFLAGS)),,-j$(TEST_JOBS))
# A switch given to them is not taken, and they say so.
FAST_IGNORED = $(strip $(foreach v,TTS STT SUM WEB,$(if $(filter command line,$(origin $(v))),$(v))))
build test:
	@scripts/test/lib/require_local.sh $@
	$(if $(FAST_IGNORED),@echo "make $@: $(FAST_IGNORED) not taken: the fast build is one configuration; make openchimed or make run takes them" >&2)
	+@$(FAST_MAKE) fast-$(if $(filter test,$@),test,build)
fast-build: daemon windows

# The daemon with the switches in force and, beside it, the data its
# read-aloud and voice input need (none without them): what `make run`, the
# full build and the manual harnesses (tools/) build.
daemon: $(BIN) $(TTS_DATA) $(STT_DATA)

# One-shot local dev server: builds the daemon (with the remembered switches),
# the data its read-aloud and voice input need, and the Linux TUI, and runs the
# daemon on port 8443 (health 8080) with sane paths + bootstrap users
# (alice/pw, bob/pw).
# It listens on 127.0.0.1 only (OPENCHIME_DEV_LOOPBACK=on), so its fixed
# passwords are not reachable from the network. Connect from another terminal:
# build/openchime-tui 127.0.0.1 8443, then sign in as alice/pw
#
# LEAN=1 runs the same daemon under the lean profile's ceiling (ARCH-4,
# REQ-210): 256 MB with no swap and one CPU, in a transient systemd user scope,
# so running out of memory ends in the kill a small box gives rather than
# swapping. An approximation of a small machine, not a copy of one. Where no
# user scope can be made it stops and says so; it never runs unconstrained.
LEAN ?= 0
LEAN_SCOPE := systemd-run --user --scope --quiet -p MemoryMax=256M -p MemorySwapMax=0 -p CPUQuota=100%
# The probe reads the limits back from inside a scope: systemd accepts a limit
# for a controller the user manager was not delegated and applies nothing, so
# a scope that starts proves nothing. 268435456 is 256M; "100000 100000" one CPU.
LEAN_PROBE := sh -c 'cg=$$(sed -n "s/^0:://p" /proc/self/cgroup); \
	[ "$$(cat /sys/fs/cgroup$$cg/memory.max 2>/dev/null)" = 268435456 ] && \
	[ "$$(cat /sys/fs/cgroup$$cg/memory.swap.max 2>/dev/null)" = 0 ] && \
	[ "$$(cat /sys/fs/cgroup$$cg/cpu.max 2>/dev/null)" = "100000 100000" ]'
run: daemon $(TUI_BIN)
	@mkdir -p /tmp/openchime-dev
	@if [ "$(LEAN)" = 1 ] && ! $(LEAN_SCOPE) $(LEAN_PROBE) >/dev/null 2>&1; then \
	   echo "make run LEAN=1: cannot create a systemd user scope here (no systemd, no user session, or cgroup v2 delegation of the memory and cpu controllers missing)." >&2; \
	   echo "The daemon was not started: it would have run without the limits. Run it on a host with a systemd user session, or in a 256 MB, one-CPU VM or container; or run make run without LEAN." >&2; \
	   exit 1; \
	 fi
	@echo "openchime dev daemon -> 127.0.0.1:8443, loopback only  (users: alice/pw, bob/pw)"
	@[ "$(LEAN)" != 1 ] || echo "lean profile: MemoryMax=256M, MemorySwapMax=0, CPUQuota=100% (one CPU), in a systemd user scope"
	@echo "connect: build/openchime-tui 127.0.0.1 8443  (sign in as alice/pw)"
	@OPENCHIME_MODE=development OPENCHIME_DATA_DIR=/tmp/openchime-dev OPENCHIME_PORT=8443 OPENCHIME_HEALTH_PORT=8080 \
	 OPENCHIME_DEV_LOOPBACK=on OPENCHIME_DEV_USERS="alice:pw:owner,bob:pw:member" $(if $(filter 1,$(LEAN)),$(LEAN_SCOPE) -- )./$(BIN)

# Everything built, except the speech models under build/kitten and
# build/moonshine: they are downloads, not builds, and cost minutes to fetch
# again. `distclean` removes those too, and the remembered features.
clean:
	rm -f $(BIN)
	find build -mindepth 1 -maxdepth 1 ! -name kitten ! -name moonshine ! -name .features -exec rm -rf {} + 2>/dev/null || true

distclean:
	rm -f $(BIN)
	rm -rf build
