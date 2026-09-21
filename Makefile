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
# workflow does pass variables on the command line (CC, OC_VERSION), so the one
# build that must not carry warnings is exactly the one best placed to drop the
# flag that forbids them. `override` is what makes this hold: a plain `+=` still
# loses to a command-line assignment, which is precisely the case being guarded.
override CFLAGS += $(WARN_CFLAGS)
LDFLAGS ?= -lpthread -lm   # -lm: SQLite FTS5 ranking (bm25) calls log()

# The default goal, stated rather than inherited from whichever rule happens to
# appear first. Without this, adding a rule above `all:` silently makes that rule
# what a bare `make` builds -- which is exactly what the SQLite rule below did,
# leaving a tree with no daemon in it and an exit status of 0.
.DEFAULT_GOAL := all

# --- SQLite (vendored, third_party/sqlite-3.53.4) -----------------------------
#
# The amalgamation, compiled into the daemon rather than linked from the host.
# The point is not the dependency count: the daemon otherwise inherits whatever
# SQLite the distro ships, INCLUDING whether FTS5 was compiled into it, and
# migrate.c creates messages_fts as a virtual table USING fts5. A host without
# that module fails at migration time, on a machine we do not control. Pinning
# the version makes it a build fact instead.
#
# Its own rule, with its own flags: 9 MB of generated C does not survive
# -Wall -Wextra -Werror, the same accommodation audio_dev.c makes for miniaudio.
SQLITE_DIR  := third_party/sqlite-3.53.4
SQLITE_INC  := -I$(SQLITE_DIR)
SQLITE_O    := build/sqlite3.o
# FTS5 is required, not tuning. THREADSAFE=1 is the system library's own default,
# so vendoring changes no concurrency semantics -- three threads hold their own
# connections (dbwriter's writer and reader, and push). The omissions are things
# this daemon provably does not use: no extension loading, no deprecated calls,
# no shared cache, and no double-quoted string literals in any of its SQL.
SQLITE_DEFS := -DSQLITE_ENABLE_FTS5 -DSQLITE_THREADSAFE=1 -DHAVE_USLEEP=1 \
               -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED \
               -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 \
               -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_OMIT_SHARED_CACHE

$(SQLITE_O): $(SQLITE_DIR)/sqlite3.c $(SQLITE_DIR)/sqlite3.h | build
	$(CC) $(filter-out $(WARN_CFLAGS),$(CFLAGS)) -w $(SQLITE_DEFS) -c $< -o $@

# Release identity (ARCH-20). The release workflow passes the release number it
# is about to publish (OC_VERSION=7); a source build leaves it unset and the
# daemon reports "dev". Only the daemon takes it — the tests link the same
# objects and have no use for it.
OC_VERSION ?=
ifneq ($(OC_VERSION),)
VERSION_DEF := -DOC_VERSION='"$(OC_VERSION)"'
else
VERSION_DEF :=
endif

BIN := openchimed

# The tree is split into three concerns: shared/ is the wire contract (linked by
# both the daemon and the client), daemon/ is the server, client/ is the app.
SHARED_SRC := shared/protocol.c shared/framebuf.c shared/tls.c shared/mention.c \
              shared/searchq.c shared/notify.c shared/url.c shared/richtext.c shared/speakable.c \
              shared/oc_mp4.c shared/e2e_hpke.c shared/e2e_sframe.c
DAEMON_SRC := daemon/main.c daemon/config.c daemon/migrate.c daemon/dbwriter.c daemon/netloop.c daemon/auth.c daemon/jwt.c daemon/joinrules.c daemon/ratelimit.c daemon/roles.c daemon/blobstore.c daemon/blob_s3.c daemon/xferpool.c daemon/storage.c daemon/sigv4.c daemon/http.c daemon/audio_sidecar.c daemon/enroll.c daemon/push.c daemon/unfurl.c daemon/voice_pick.c
SRC        := $(SHARED_SRC) $(DAEMON_SRC)
HDRS       := $(wildcard shared/*.h daemon/*.h)

# Vendored, pinned mbedTLS (scripts/build_mbedtls.sh) — one version across local
# builds, CI, the release, and the container image. Link order matters for static
# archives: tls -> x509 -> crypto.
MBEDTLS_DIR  := third_party/mbedtls-3.6.2
MBEDTLS_INC  := $(MBEDTLS_DIR)/include
MBEDTLS_A    := $(MBEDTLS_DIR)/library/libmbedtls.a
MBEDTLS_LIBS := $(MBEDTLS_DIR)/library/libmbedtls.a \
                $(MBEDTLS_DIR)/library/libmbedx509.a \
                $(MBEDTLS_DIR)/library/libmbedcrypto.a

INC := -Ishared -Idaemon -Ithird_party/jsmn -I$(MBEDTLS_INC)

# Every source except the daemon entry point; the test binary links these and
# calls their public APIs (no per-test binaries, no unity #include of .c).
APP_SRC   := $(SHARED_SRC) $(filter-out daemon/main.c,$(DAEMON_SRC))
# e2e_client is a standalone black-box tool (its own main), not part of the
# single in-process test binary.
TEST_SRC  := $(filter-out tests/e2e_client.c tests/demo_client.c tests/bench_load.c,$(wildcard tests/*.c))
TEST_BIN  := build/tests

# --- Client app-core (ARCH-74) ------------------------------------------------
# The shared, frontend-agnostic core: the net thread + the two UI<->net queues +
# the view-model/reducers + the oc_client facade. It has no main of its own — the
# headless test (make test) links and drives it against an in-process daemon, and
# frontends (a TUI first, then native GUIs) link it too. See docs/CLIENT.md.
CORE_SRC := $(wildcard client/core/*.c)
CORE_INC := -Iclient/core

# --- Video message media (ARCH-110) -------------------------------------------
# Capture, the audio device layer, the codec wrappers, the MP4 muxer/demuxer, the
# recorder and the player. Kept out of CORE_SRC so a client that does not record
# or play video (the TUI) links no codec. libvpx and libopus are fetched and built
# pinned (scripts/build_libvpx.sh, scripts/build_opus.sh), natively for `make
# test` and cross-built for the Win32 client.
MEDIA_SRC  := $(wildcard client/core/media/*.c)
MEDIA_HDRS := $(wildcard client/core/media/*.h)
LIBVPX_DIR := third_party/libvpx-1.17.0
OPUS_DIR   := third_party/opus-1.6.1
LIBVPX_A   := $(LIBVPX_DIR)/lib/libvpx.a
OPUS_A     := $(OPUS_DIR)/lib/libopus.a
# speexdsp's echo canceller sits behind the processor seam (AUDIO.md §3.3), for
# voice input first and calls after (ARCH-112); fetched and built pinned
# (scripts/build_speexdsp.sh) like the codecs.
SPEEXDSP_DIR := third_party/speexdsp-1.2.1
SPEEXDSP_A   := $(SPEEXDSP_DIR)/lib/libspeexdsp.a
MEDIA_INC  := -Iclient/core/media -I$(LIBVPX_DIR)/include -I$(OPUS_DIR)/include -I$(SPEEXDSP_DIR)/include
MEDIA_LIBS := $(LIBVPX_A) $(OPUS_A) $(SPEEXDSP_A) -ldl -lm

# --- Voice input (ARCH-112) ---------------------------------------------------
# The dictation session, the libfvad segmenter (vendored source,
# third_party/libfvad) and the capture thread, over the media library's device
# layer and processor. Linked by the Win32 client and the tests.
VOICE_SRC  := $(wildcard client/core/voice/*.c)
VOICE_HDRS := $(wildcard client/core/voice/*.h)
VOICE_INC  := -Iclient/core/voice -Ithird_party/libfvad/include

# --- Calls (ARCH-113, docs/CALLS.md) -----------------------------------------
# The media engine behind the core's oc_call_media seam: the microphone through
# the canceller and speexdsp's preprocessor, Opus, SFrame, the relay socket, the
# jitter buffers and the mixer. Linked by the Win32 client and the tests; the
# core does the signaling and the keys, and links no codec.
CALL_SRC  := $(wildcard client/core/call/*.c)
CALL_HDRS := $(wildcard client/core/call/*.h)
CALL_INC  := -Iclient/core/call

# --- ttskit (read-aloud pronunciation, ARCH-111) -----------------------------
# English text to the IPA a voice model reads: a CMUdict-derived dictionary and a
# trained guesser, both committed as memory-mapped data in ttskit/data. Pure C, no
# dependencies; linked into the daemon and covered by `make test`. tts_pack
# builds the data files (scripts/build_ttskit_data.sh) and says words with them.
# See docs/TTSKIT.md.
TTSKIT_SRC := $(filter-out ttskit/tts_pack.c,$(wildcard ttskit/*.c))
TTSKIT_INC := -Ittskit

# --- sdltext (portable text layer) --------------------------------------------
# One text API for every graphical client: layout, measurement, hit-testing,
# byte-offset range styling. The portable core (the byte<->UTF-16 offset map)
# compiles everywhere and is covered by `make test`; the DirectWrite backend
# compiles only into Windows builds. See sdltext/sdltext.h.
SDLTEXT_COMMON := sdltext/st_common.c
SDLTEXT_WIN    := sdltext/st_common.c sdltext/st_dwrite.c
SDLTEXT_INC    := -Isdltext

# --- Client TUI frontend (ARCH-75) --------------------------------------------
# termbox2 (cell grid + input) + utf8proc (Unicode width/grapheme), both vendored
# as committed MIT single-file source. Builds on the host like the daemon.
TUI_SRC   := $(wildcard client/tui/*.c) $(wildcard client/shared/secret_*.c)
# tuikit — the in-tree TUI toolbox (terminal layer + widgets + formatting). Owns
# the termbox2 instantiation + the Windows console backend (ARCH-83). Linked into
# both the POSIX and Windows TUI.
TUIKIT_SRC := $(filter-out tuikit/demo.c,$(wildcard tuikit/*.c))
TUIKIT_INC := -Ituikit
UTF8PROC  := third_party/utf8proc/utf8proc.c
# The core's local store (client/core/store.c) embeds NO database engine
# (ARCH-88/REQ-201): the credential store holds the token + pin, and the cache,
# outbox and workspace book are plain files. So a client links no sqlite and no
# migration runner — STORE_DEPS is gone with them.
# The platform credential backend (client/shared/secret_*.c, one entry point in
# secret_os.h): libsecret on Linux when the dev package is present, Windows
# Credential Manager on the Windows builds. Without libsecret it compiles a stub
# that reports "no keyring" — `make tui` still builds, but that machine then
# persists no session token at all (a credential is never written to plaintext).
ifeq ($(shell pkg-config --exists libsecret-1 && echo yes),yes)
  SECRET_CFLAGS := $(shell pkg-config --cflags libsecret-1) -DOC_HAVE_LIBSECRET
  SECRET_LIBS   := $(shell pkg-config --libs libsecret-1)
else
  SECRET_CFLAGS :=
  SECRET_LIBS   :=
endif
TUI_INC   := $(CORE_INC) -Iclient/tui -Iclient/shared -Ithird_party/termbox2 -Ithird_party/utf8proc
TUI_BIN   := build/openchime-tui

.PHONY: all run test check-opcodes check-refs check-release-cc core tui bench clean s3-smoke windows-tui windows-gui tuikit-demo tts_pack demo-client

all: $(BIN)

# One-shot local dev server: builds the daemon + TUI and runs the daemon on
# 127.0.0.1:8443 with sane paths + bootstrap users (alice/pw, bob/pw). Connect
# from another terminal: build/openchime-tui 127.0.0.1 8443 alice:pw
run: $(BIN) $(TUI_BIN)
	@mkdir -p /tmp/openchime-dev/blobs
	@echo "openchime dev daemon -> 127.0.0.1:8443  (users: alice/pw, bob/pw)"
	@echo "connect: build/openchime-tui 127.0.0.1 8443 alice:pw"
	@OPENCHIME_DB_PATH=/tmp/openchime-dev/db OPENCHIME_BLOB_DIR=/tmp/openchime-dev/blobs \
	 OPENCHIME_TLS_CERT=/tmp/openchime-dev/cert.pem OPENCHIME_TLS_KEY=/tmp/openchime-dev/key.pem \
	 OPENCHIME_PROTO_PORT=8443 OPENCHIME_HEALTH_PORT=8080 \
	 OPENCHIME_BOOTSTRAP_USERS="alice:pw:owner,bob:pw:member" ./$(BIN)

# --- Read-aloud in the daemon (ARCH-111) ---------------------------------------
# On by default: the daemon carries its own speech synthesis. ttskit and a minimal
# ONNX Runtime built from source as one static archive are compiled in; the voice
# model, its voices and the pronunciation data are DATA, assembled into voices/
# beside the binary with a manifest the daemon checks at startup. Absent or
# mismatched data turns read-aloud off rather than stopping the daemon. voices/
# beside openchimed is the same layout the tarball ships, so a source build finds
# its data the way an extracted release does. The first build compiles ONNX
# Runtime (about 20 minutes) and fetches the model; both are kept under
# third_party/ and build/. `make TTS=0` builds without.
TTS ?= 1
# What `make test` links of read-aloud whatever TTS is: the render path and worker
# (driven by a stub engine) and the tokenizer -- everything but the model.
TTS_TEST_SRC := daemon/tts_render.c daemon/tts_worker.c daemon/tts_kitten_tokens.c daemon/tts_data.c
ORT_DIR   := third_party/onnxruntime-1.30.0
ORT_A     := $(ORT_DIR)/lib/libonnxruntime.a
KITTEN    := build/kitten/kitten.ort
ifeq ($(TTS),1)
TTS_SRC   := daemon/tts.c daemon/tts_render.c daemon/tts_worker.c daemon/tts_kitten.c daemon/tts_kitten_tokens.c daemon/tts_data.c $(TTSKIT_SRC)
TTS_DEPS  := $(ORT_A) $(OPUS_A) $(wildcard ttskit/*.h)
TTS_DATA  := voices/manifest
TTS_FLAGS := -DOC_TTS $(TTSKIT_INC) -I$(ORT_DIR)/include -I$(OPUS_DIR)/include
# ONNX Runtime is C++: its standard library and runtime support are linked in
# statically, not required of the host. That is GCC's libstdc++ by default; the
# release, which compiles everything with zig, passes TTS_CXXLIB=-lc++ for zig's
# bundled libc++ (the library ONNX Runtime was compiled against there).
# One of its assembly kernels lacks the GNU-stack note, which would otherwise mark
# the whole daemon's stack executable; the stack stays non-executable.
TTS_CXXLIB ?= -static-libstdc++ -static-libgcc -Wl,-Bstatic -lstdc++ -Wl,-Bdynamic -ldl
TTS_LIBS  := $(OPUS_A)
endif

# --- Voice input in the daemon (ARCH-112) ---------------------------------------
# On by default, like read-aloud: the recognizer's code is compiled in and runs on
# the same minimal ONNX Runtime; Moonshine's published files are DATA, fetched at
# pinned SHA-256s into build/moonshine and assembled into stt/ beside the binary
# with a manifest the daemon checks at startup. Absent or mismatched data turns
# voice input off, and read-aloud with it untouched. `make STT=0` builds without.
STT ?= 1
# What `make test` links of voice input whatever STT is: the worker (driven by a
# stub engine) and the tokenizer -- everything but the model.
STT_TEST_SRC := daemon/stt_worker.c daemon/stt_tokens.c daemon/stt_mentions.c
ifeq ($(STT),1)
STT_SRC   := daemon/stt.c daemon/stt_moonshine.c daemon/stt_worker.c daemon/stt_tokens.c daemon/stt_mentions.c
STT_DEPS  := $(ORT_A)
STT_DATA  := stt/manifest
STT_FLAGS := -DOC_STT -I$(ORT_DIR)/include
ifneq ($(TTS),1)
STT_SRC   += daemon/tts_data.c
endif
endif

# ONNX Runtime and its C++ runtime, once, for whichever speech features are in.
ifneq ($(filter 1,$(TTS) $(STT)),)
TTS_CXXLIB ?= -static-libstdc++ -static-libgcc -Wl,-Bstatic -lstdc++ -Wl,-Bdynamic -ldl
ORT_LIBS  := $(ORT_A) $(TTS_CXXLIB) -lm -Wl,--gc-sections -Wl,-z,noexecstack
endif

$(BIN): $(SRC) $(TTS_SRC) $(STT_SRC) $(MBEDTLS_A) $(HDRS) $(TTS_DEPS) $(STT_DEPS) $(SQLITE_O)
	$(CC) $(CFLAGS) $(VERSION_DEF) $(INC) $(SQLITE_INC) $(TTS_FLAGS) $(STT_FLAGS) -o $@ $(SRC) $(TTS_SRC) $(STT_SRC) $(SQLITE_O) $(MBEDTLS_LIBS) $(TTS_LIBS) $(ORT_LIBS) $(LDFLAGS)

# The data directory, and its manifest written by the daemon just built -- so the
# version it names is exactly the version compiled in. Rebuilt whenever the daemon
# is, which is whenever that version could have changed.
voices/manifest: $(BIN) $(KITTEN) build/kitten/voices.npz ttskit/data/en-US/lexicon.bin ttskit/data/en-US/guesses.bin
	mkdir -p voices/en-US
	cp $(KITTEN) build/kitten/voices.npz voices/
	cp ttskit/data/en-US/lexicon.bin ttskit/data/en-US/guesses.bin voices/en-US/
	$(abspath $(BIN)) --tts-manifest voices
# Voice input's data directory, the same way: Moonshine's files and a manifest
# written by the daemon just built.
stt/manifest: $(BIN) build/moonshine/.done
	mkdir -p stt
	cp $(addprefix build/moonshine/,frontend.model.ort frontend.weights.ort encoder.ort adapter.ort cross_kv.ort decoder_kv.ort tokenizer.bin streaming_config.json) stt/
	$(abspath $(BIN)) --stt-manifest stt
# Added here rather than on `all:` above, which make reads before TTS_DATA exists
# and would expand to nothing.
all: $(TTS_DATA) $(STT_DATA)

$(ORT_A): daemon/ort.ops.config
	scripts/build_onnxruntime.sh
$(KITTEN) build/kitten/voices.npz: scripts/build_kitten.sh scripts/tts_convert.c
	scripts/build_kitten.sh
build/moonshine/.done: scripts/build_moonshine.sh
	scripts/build_moonshine.sh

$(MBEDTLS_A):
	scripts/build_mbedtls.sh
$(LIBVPX_A):
	scripts/build_libvpx.sh native
$(OPUS_A):
	scripts/build_opus.sh native
$(SPEEXDSP_A):
	scripts/build_speexdsp.sh native

# The wire contract's one static invariant: no two message types share an opcode.
# A source check rather than a C test, because a C test cannot enumerate an enum.
check-opcodes:
	scripts/check_opcodes.sh shared/protocol.h

# No file cites an issue by number (CONTRIBUTING.md). Like check-opcodes this is
# a source check rather than a C test: what it inspects is the text of the tree,
# not the behaviour of the program. A commit cites the issue it closes; a file
# says the thing instead, because a number in a comment rots silently the moment
# the issue does.
check-refs:
	scripts/check_refs.sh

# The daemon's sources through the RELEASE's compiler, with the release's flags,
# stopping short of the link (the release links an ONNX Runtime built by the same
# compiler, which CI does not build). A newer clang refuses things CI's accepts --
# a declaration after a label, a C23 extension, broke a release this way -- and
# the release is the wrong place to learn it. `RELEASE_CC` is what release.yml
# compiles with.
RELEASE_CC ?= /opt/zig/zig cc -target x86_64-linux-gnu.2.34
check-release-cc: $(MBEDTLS_A) $(TTS_DEPS) $(STT_DEPS)
	@for f in $(SRC) $(TTS_SRC) $(STT_SRC); do \
	  $(RELEASE_CC) $(CFLAGS) $(VERSION_DEF) $(INC) $(SQLITE_INC) $(TTS_FLAGS) $(STT_FLAGS) -c -o /dev/null $$f || exit 1; \
	done; echo "check-release-cc: $(words $(SRC) $(TTS_SRC) $(STT_SRC)) sources clean"

# Unit + in-process integration tests, one binary (docs/TESTING.md §2). Built
# -O0 -g; a non-zero exit fails the build and CI.
test: check-opcodes check-refs $(TEST_BIN)
	./$(TEST_BIN)

# theme.c is GUI source and is compiled in anyway: it is colour arithmetic with
# one Windows call behind an #ifdef, and the contrast guarantee it carries has to
# be asserted somewhere that RUNS. The audit that would otherwise check it needs
# a Windows host and a developer who remembers; this needs neither.
THEME_SRC := client/gui/win32/theme.c

$(TEST_BIN): $(TEST_SRC) $(APP_SRC) $(CORE_SRC) $(MEDIA_SRC) $(VOICE_SRC) $(VOICE_HDRS) $(CALL_SRC) $(CALL_HDRS) $(SDLTEXT_COMMON) $(THEME_SRC) $(TTSKIT_SRC) $(TTS_TEST_SRC) $(STT_TEST_SRC) $(HDRS) $(MEDIA_HDRS) $(wildcard tests/*.h client/core/*.h sdltext/*.h ttskit/*.h daemon/tts_*.h daemon/stt_*.h client/gui/win32/theme.h) $(MBEDTLS_A) $(LIBVPX_A) $(OPUS_A) $(SPEEXDSP_A) $(SQLITE_O) | build
	$(CC) $(CFLAGS) -O0 -g $(INC) $(SQLITE_INC) $(CORE_INC) $(MEDIA_INC) $(VOICE_INC) $(CALL_INC) $(TTSKIT_INC) -DOC_TTS -DOC_STT -Itests -Iclient/gui/win32 \
	    $(TEST_SRC) $(APP_SRC) $(CORE_SRC) $(MEDIA_SRC) $(VOICE_SRC) $(CALL_SRC) $(SDLTEXT_COMMON) $(THEME_SRC) $(TTSKIT_SRC) $(TTS_TEST_SRC) $(STT_TEST_SRC) $(SQLITE_O) $(MBEDTLS_LIBS) $(MEDIA_LIBS) -lresolv -lpthread -lm -o $@

# There is no `integration` target any more. It ran Scripts/test-integration.sh,
# which drove the daemon through a Docker Compose stack; the project no longer
# uses Docker anywhere, and the script was deleted rather than reimplemented. The
# two assertions it made now live in the `integration` job of
# .github/workflows/ci.yml, against a natively-run daemon. `make build/e2e_client`
# still builds the client that job drives, if you want to point it at a daemon of
# your own. See docs/TESTING.md §3.

# The e2e client links only the shared wire modules (no daemon internals).
build/e2e_client: tests/e2e_client.c $(SHARED_SRC) $(wildcard shared/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O0 -g -Ishared -I$(MBEDTLS_INC) \
	    tests/e2e_client.c $(SHARED_SRC) $(MBEDTLS_LIBS) -o $@

# Flexible black-box client for the local federated demo (register a device token /
# send a message against a running daemon). Same shared-only linkage as e2e_client.
demo-client: build/demo_client
build/demo_client: tests/demo_client.c $(SHARED_SRC) $(wildcard shared/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O0 -g -Ishared -I$(MBEDTLS_INC) \
	    tests/demo_client.c $(SHARED_SRC) $(MBEDTLS_LIBS) -o $@

# Capacity benchmark load client (REQ-210/211); driven by Scripts/bench.sh.
# Links only the shared wire modules, like the e2e client.
bench: build/bench_load
build/bench_load: tests/bench_load.c $(SHARED_SRC) $(wildcard shared/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O2 -Ishared -I$(MBEDTLS_INC) \
	    tests/bench_load.c $(SHARED_SRC) $(MBEDTLS_LIBS) -lpthread -o $@

# Standalone compile check for the client app-core (no frontend, no main). The
# headless test binary (make test) is the real coverage; this just proves the
# core still compiles on its own against the shared wire code.
# Manual S3 smoke test (tests/manual/s3_smoke.c) -- NOT part of `make test`.
# Needs live S3 credentials in the environment; see the header of that file.
s3-smoke: tests/manual/s3_smoke.c daemon/blobstore.c daemon/blob_s3.c daemon/sigv4.c shared/tls.c $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) $(INC) tests/manual/s3_smoke.c daemon/blobstore.c \
	    daemon/blob_s3.c daemon/sigv4.c shared/tls.c $(MBEDTLS_LIBS) -lpthread -o build/s3_smoke
	@echo "built build/s3_smoke -- needs OPENCHIME_S3_* credentials to run"

core: $(CORE_SRC) $(SHARED_SRC) $(wildcard client/core/*.h shared/*.h) $(MBEDTLS_A) | build
	mkdir -p build/core
	for f in $(CORE_SRC); do \
	    $(CC) $(CFLAGS) $(INC) $(CORE_INC) -c $$f -o build/core/$$(basename $$f .c).o || exit 1; \
	done

# The TUI: app-core + shared wire + termbox2/utf8proc. -Wno-unused-result relaxes
# one warning from the vendored termbox2 header (its read/write/strerror_r calls).
tui: $(TUI_BIN)
$(TUI_BIN): $(TUI_SRC) $(TUIKIT_SRC) $(CORE_SRC) $(SHARED_SRC) $(UTF8PROC) \
            $(wildcard client/tui/*.h tuikit/*.h client/core/*.h shared/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -Wno-unused-result $(INC) $(TUI_INC) $(TUIKIT_INC) $(SECRET_CFLAGS) \
	    $(TUI_SRC) $(TUIKIT_SRC) $(CORE_SRC) $(SHARED_SRC) $(UTF8PROC) $(MBEDTLS_LIBS) -lresolv -lpthread $(SECRET_LIBS) -o $@


# --- Windows TUI (ARCH-81) ----------------------------------------------------
# Cross-compiled with mingw-w64 to a standalone .exe. Uses the Windows mbedTLS
# (third_party/mbedtls-3.6.2-win). No sqlite: a client embeds no database engine
# (ARCH-88). The termbox2 backend is tuikit/tk_term.c (Console API); the
# core seams (threads/DNS/RNG) are in shared/oc_thread.h + resolve.c + net.c.
WINCC     ?= x86_64-w64-mingw32-gcc
WINDRES   ?= x86_64-w64-mingw32-windres
WINOBJCOPY ?= x86_64-w64-mingw32-objcopy

# Both .exe files carry a VERSIONINFO resource from client/shared/res. Only the
# number is passed; the .rc builds the display string from it, for the reason
# recorded there. OC_VERSION is a bare integer or empty, and VERSIONINFO has no
# spelling for "dev", so an unset version stamps 0.
#
# make tracks file times, not variables, so changing OC_VERSION on a tree that
# already built leaves the old number in build/*_res.o. CI checks out clean and
# never sees this; a local rebuild at a new version wants `make clean` first.
OC_VERSION_NUM := $(if $(OC_VERSION),$(OC_VERSION),0)
WIN_RES_SRC  := client/shared/res/version_info.rc
WINDRES_ARGS := -I client/shared/res --define OC_VERSION_NUM=$(OC_VERSION_NUM)
MBEDTLS_WIN := third_party/mbedtls-3.6.2-win
WIN_MBEDLIBS := $(MBEDTLS_WIN)/library/libmbedtls.a \
                $(MBEDTLS_WIN)/library/libmbedx509.a \
                $(MBEDTLS_WIN)/library/libmbedcrypto.a
WIN_TUI_BIN := build/openchime-tui.exe
# -g: debug info, so a crash RVA from the report (see crash_filter) resolves to a
# file and line via addr2line. Without it the report can only say "somewhere in
# winmain.c", which is not a lead. No runtime cost; strip on release if size matters.
# The Windows flags are their own variable rather than an addition to CFLAGS, so
# they carry WARN_CFLAGS explicitly. They must: the cross build is where the two
# implicit float declarations hid, because nothing on the native side compiles
# winmain.c at all.
WIN_CFLAGS := -std=c99 $(WARN_CFLAGS) -O2 -g -D_WIN32_WINNT=0x0601 -DUTF8PROC_STATIC
WIN_INC := -Ishared -Idaemon -Ithird_party/jsmn -I$(MBEDTLS_WIN)/include \
           $(CORE_INC) -Iclient/tui -Iclient/shared -Ituikit -Ithird_party/termbox2 -Ithird_party/utf8proc

# Version metadata only -- no icon, which for a console program comes from the
# host terminal window rather than the image.
WIN_TUI_RES := build/openchime_tui_res.o
$(WIN_TUI_RES): client/tui/res/openchime_tui.rc $(WIN_RES_SRC) | build
	$(WINDRES) $(WINDRES_ARGS) $< -O coff -o $@

windows-tui: $(WIN_TUI_BIN)
$(WIN_TUI_BIN): $(TUI_SRC) $(TUIKIT_SRC) $(CORE_SRC) $(SHARED_SRC) $(UTF8PROC) $(WIN_TUI_RES) \
                $(wildcard client/tui/*.h tuikit/*.h client/core/*.h shared/*.h) $(WIN_MBEDLIBS) | build
	$(WINCC) $(WIN_CFLAGS) -Wno-unused-result $(WIN_INC) \
	    $(TUI_SRC) $(TUIKIT_SRC) $(CORE_SRC) $(SHARED_SRC) $(UTF8PROC) $(WIN_TUI_RES) \
	    $(WIN_MBEDLIBS) -lws2_32 -ldnsapi -lbcrypt -lole32 -ladvapi32 -static -o $@


# The native Windows GUI (Win32 + Direct2D/DirectWrite/WIC, pure C — ARCH-80/82)
# over the same shared app-core. Mirrors windows-tui (core+shared+mbedtls-win)
# but compiles the client/gui/win32 sources instead of the
# TUI/tuikit stack and links the Direct2D stack. -municode gives the wWinMain
# Unicode entry point; -mwindows selects the GUI subsystem (no console).
SDL3_VERSION := 3.4.14
SDL3_WIN     := third_party/sdl3-$(SDL3_VERSION)-win
SDL3_WIN_LIB := $(SDL3_WIN)/lib/libSDL3.a
# The OS libraries a static SDL3 needs beyond what the GUI already links.
WIN_SDL_SYSLIBS := -lm -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 \
                   -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8
GFX_SRC := client/gui/gfx/gfx_sdl.c client/gui/gfx/gfx_icons.c

$(SDL3_WIN_LIB):
	scripts/build_sdl3_windows.sh

WIN_GUI_BIN := build/openchime.exe
# Video messages (ARCH-110): the cross-built codecs, and Media Foundation for the
# camera. The native rules above build Linux archives only, so like mbedTLS these
# have a rule of their own here.
WIN_LIBVPX_A := third_party/libvpx-1.17.0-win/lib/libvpx.a
WIN_OPUS_A   := third_party/opus-1.6.1-win/lib/libopus.a
WIN_SPEEXDSP_A := third_party/speexdsp-1.2.1-win/lib/libspeexdsp.a
WIN_MEDIA_A  := $(WIN_LIBVPX_A) $(WIN_OPUS_A) $(WIN_SPEEXDSP_A)
WIN_MEDIA_INC := -Iclient/core/media -Ithird_party/libvpx-1.17.0-win/include -Ithird_party/opus-1.6.1-win/include \
                 -Ithird_party/speexdsp-1.2.1-win/include $(VOICE_INC)
# -lpthread: libvpx's mingw build threads through winpthreads (statically linked
# here, like everything else, so no DLL ships beside the .exe).
WIN_MEDIA_SYSLIBS := -lmfplat -lmfreadwrite -lmf -lmfuuid -lole32 -luuid -lpthread
$(WIN_LIBVPX_A):
	scripts/build_libvpx.sh windows
$(WIN_OPUS_A):
	scripts/build_opus.sh windows
$(WIN_SPEEXDSP_A):
	scripts/build_speexdsp.sh windows
# Debug symbols, split out of the shipped binary (see the strip step below).
WIN_GUI_SYMS := build/openchime.debug
GUI_SRC := $(wildcard client/gui/win32/*.c) client/shared/icons.c client/shared/secret_win.c \
           $(SDLTEXT_WIN) $(GFX_SRC)
WIN_GUI_INC := -Ishared -Idaemon -Ithird_party/jsmn -I$(MBEDTLS_WIN)/include \
               $(CORE_INC) $(CALL_INC) -Iclient/gui/win32 -Iclient/shared \
               $(SDLTEXT_INC) -Iclient/gui/gfx -I$(SDL3_WIN)/include

# Resources (app icon + VERSIONINFO). Regenerate the .ico with
# scripts/gen_appicon.py.
WIN_GUI_RES := build/openchime_res.o
$(WIN_GUI_RES): client/gui/win32/res/openchime.rc client/gui/win32/res/openchime.ico \
                client/gui/win32/res/openchime_res.h $(WIN_RES_SRC) | build
	$(WINDRES) -I client/gui/win32/res $(WINDRES_ARGS) $< -O coff -o $@

windows-gui: $(WIN_GUI_BIN)
$(WIN_GUI_BIN): $(GUI_SRC) $(CORE_SRC) $(MEDIA_SRC) $(VOICE_SRC) $(CALL_SRC) $(SHARED_SRC) $(WIN_GUI_RES) \
                $(wildcard client/gui/win32/*.h client/core/*.h shared/*.h sdltext/*.h client/gui/gfx/*.h) $(MEDIA_HDRS) $(VOICE_HDRS) $(CALL_HDRS) \
                $(WIN_MBEDLIBS) $(SDL3_WIN_LIB) $(WIN_MEDIA_A) | build
	$(WINCC) $(WIN_CFLAGS) -Wno-unused-result -municode -mwindows $(WIN_GUI_INC) $(WIN_MEDIA_INC) -Iclient/gui/win32/res \
	    $(GUI_SRC) $(CORE_SRC) $(MEDIA_SRC) $(VOICE_SRC) $(CALL_SRC) $(SHARED_SRC) $(WIN_GUI_RES) \
	    $(WIN_MBEDLIBS) $(WIN_MEDIA_A) -L$(SDL3_WIN)/lib -lSDL3 -lws2_32 -ldnsapi -lbcrypt -lcomdlg32 \
	    -ld2d1 -ldwrite -lwindowscodecs -ldwmapi -limm32 $(WIN_MEDIA_SYSLIBS) $(WIN_SDL_SYSLIBS) -static -o $@
# Split the debug info out rather than discarding it. The client writes real
# minidumps on a crash (crash_filter, winmain.c), and symbolicating a mingw
# build needs its DWARF -- a plain strip would shrink the download by trading
# away every future crash report. So: keep the symbols beside the binary, strip
# the shipped one, and record a debuglink so a debugger loads them back.
	$(WINOBJCOPY) --only-keep-debug $@ $(WIN_GUI_SYMS)
	$(WINOBJCOPY) --strip-debug --strip-unneeded $@
	$(WINOBJCOPY) --add-gnu-debuglink=$(WIN_GUI_SYMS) $@

# The DirectWrite backend's test program (sdltext/st_test_win.c). A console
# .exe whose exit code is its failure count; CI cross-compiles it (the compile
# is itself the header/vtable check mingw keeps honest), a Windows host — or
# WSL interop — runs it. docs/TESTING.md records the split.
WIN_SDLTEXT_TEST := build/sdltext_test.exe
windows-sdltext-test: $(WIN_SDLTEXT_TEST)
$(WIN_SDLTEXT_TEST): $(SDLTEXT_WIN) sdltext/st_test_win.c $(wildcard sdltext/*.h) | build
	$(WINCC) $(WIN_CFLAGS) $(SDLTEXT_INC) \
	    $(SDLTEXT_WIN) sdltext/st_test_win.c \
	    -ld2d1 -ldwrite -lwindowscodecs -lole32 -luuid -static -o $@

# --- oc_gfx (portable primitives over SDL3) -----------------------------------
# The drawing layer the GUI's portable application code targets: fills,
# rounded rects, clips, lines, textures and the Lucide stroke tessellator,
# over an SDL3 renderer. SDL3 is the "fetched at build" vendoring class
# (docs/VENDORS.md §2), pinned + cross-built by scripts/build_sdl3_windows.sh,
# statically linked like everything else.

# The gfx test program: SDL's software renderer on a plain surface — no
# window, no GPU — asserting pixels. Exit code is the failure count
# (docs/TESTING.md, the platform-backend exception).
WIN_GFX_TEST := build/gfx_test.exe
windows-gfx-test: $(WIN_GFX_TEST)
$(WIN_GFX_TEST): $(GFX_SRC) client/gui/gfx/gfx_test_win.c client/shared/icons.c \
                 $(wildcard client/gui/gfx/*.h) $(SDL3_WIN_LIB) | build
	$(WINCC) $(WIN_CFLAGS) -Iclient/gui/gfx -Iclient/shared -I$(SDL3_WIN)/include \
	    $(GFX_SRC) client/gui/gfx/gfx_test_win.c client/shared/icons.c \
	    -L$(SDL3_WIN)/lib -lSDL3 $(WIN_SDL_SYSLIBS) -static -o $@

# --- tuikit demo (ARCH-83) ----------------------------------------------------
# Standalone harness exercising every tuikit widget — no core, no daemon, no TLS.
# The toolbox's own smoke test.
tuikit-demo: build/tuikit-demo
build/tuikit-demo: $(TUIKIT_SRC) tuikit/demo.c $(UTF8PROC) $(wildcard tuikit/*.h) | build
	$(CC) $(CFLAGS) -Wno-unused-result $(TUIKIT_INC) -Ithird_party/termbox2 -Ithird_party/utf8proc \
	    $(TUIKIT_SRC) tuikit/demo.c $(UTF8PROC) -o $@

# --- tts_pack (ttskit's data tool) --------------------------------------------
tts_pack: build/tts_pack
build/tts_pack: $(TTSKIT_SRC) ttskit/tts_pack.c $(wildcard ttskit/*.h) | build
	$(CC) $(CFLAGS) $(TTSKIT_INC) $(TTSKIT_SRC) ttskit/tts_pack.c -lm -o $@

build:
	mkdir -p build

clean:
	rm -f $(BIN)
	rm -rf build
