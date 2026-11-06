# The test binaries and the fast suite (docs/TESTING.md), the release-compiler
# pass, and the tool binaries a person builds by path. Every test is a script
# in scripts/test/fast/ or scripts/test/full/; a target builds what the scripts
# need and calls them, and the workflows call only the targets.

# Unit + in-process integration tests, one binary (docs/TESTING.md §2). Built
# -O0 -g; a non-zero exit fails the run. The suites run side by side,
# as many at once as there are CPUs (OC_TEST_JOBS, tests/main.c): they are nearly
# all waiting, so the run takes about as long as its slowest suite. The binary
# always carries read-aloud's and voice input's test paths (stub engines) and
# never needs ONNX Runtime, llama.cpp or Emscripten, whatever the switches say.
TEST_JOBS ?= $(shell nproc 2>/dev/null || echo 4)
TEST_INC  := $(INC) $(SQLITE_INC) $(CORE_INC) $(MEDIA_INC) $(VOICE_INC) $(CALL_INC) $(TTSKIT_INC) -DOC_TTS -DOC_STT \
             -Itests -Iclient/gui/app -Iclient/gui/platform -Iclient/shared -Ituikit -Ithird_party/qrcodegen
# Sorted: the daemon and the text layout both carry utf8proc, compiled once.
TEST_ALL_SRC := $(sort $(TEST_SRC) $(APP_SRC) $(CORE_SRC) $(MEDIA_SRC) $(VOICE_SRC) $(CALL_SRC) $(SDLTEXT_COMMON) $(THEME_SRC) \
                $(TTSKIT_SRC) $(TTS_TEST_SRC) $(STT_TEST_SRC) $(QR_TEST_SRC) $(OSN_TEST_SRC))
# One object per source, in a tree of its own (mk/sources.mk, "Objects"),
# compiled with the flags the binary always had; the link takes them in the
# sorted order above, then SQLite and Argon2 (the daemon's own objects) and the
# vendored libraries, which only have to exist before anything compiles.
TEST_CC     = $(CC) $(CFLAGS) -O0 -g $(TEST_INC)
TEST_OBJS   := $(call oc_objs,$(OBJ_DIR)/test,$(TEST_ALL_SRC))
TEST_VENDOR := $(MBEDTLS_A) $(LIBVPX_A) $(OPUS_A) $(SPEEXDSP_A)
$(eval $(call oc_obj_rules,$(OBJ_DIR)/test,TEST_CC,$(CC_STAMP),$(TEST_VENDOR)))
-include $(TEST_OBJS:.o=.d)
# --- The fast suite (docs/TESTING.md) -----------------------------------------
# `make test` (Makefile) runs this in a make of the fast build's own: the fast
# build and the test binary, then the source checks, actionlint on the
# workflows and the suites SUITES[] marks FAST, and on a pass the tree it
# passed on, recorded for the pre-push hook (scripts/test/lib/tested_tree.sh).
# The full suite is mk/full.mk.
fast-test: fast-build $(TEST_BIN)
	scripts/test/fast/source.sh
	scripts/test/fast/actionlint.sh
	OC_TEST_JOBS="$${OC_TEST_JOBS:-$(TEST_JOBS)}" scripts/test/fast/suites.sh
	@scripts/test/lib/tested_tree.sh record

TSAN_BIN := build/tests-tsan
# pthread_create is wrapped in the plain test binary, so the runner sees which
# suites start threads and fails one SUITES[] does not mark THREADS
# (tests/main.c).
$(TEST_BIN): $(TEST_OBJS) $(SQLITE_O) $(ARGON2_O) $(TEST_VENDOR) $(CC_STAMP) | build-dir
	$(CC) $(CFLAGS) -O0 -g $(TEST_OBJS) $(SQLITE_O) $(ARGON2_O) $(MBEDTLS_LIBS) $(MEDIA_LIBS) -Wl,--wrap=pthread_create -lresolv -lpthread -lm -o $@
# The same test binary under ThreadSanitizer (scripts/test/full/tsan.sh runs
# it). The vendored libraries are not instrumented.
TSAN_CC   = $(CC) $(CFLAGS) -O1 -g -fsanitize=thread $(TEST_INC)
TSAN_OBJS := $(call oc_objs,$(OBJ_DIR)/tsan,$(TEST_ALL_SRC))
$(eval $(call oc_obj_rules,$(OBJ_DIR)/tsan,TSAN_CC,$(CC_STAMP),$(TEST_VENDOR)))
-include $(TSAN_OBJS:.o=.d)
$(TSAN_BIN): $(TSAN_OBJS) $(SQLITE_O) $(ARGON2_O) $(TEST_VENDOR) $(CC_STAMP) | build-dir
	$(CC) $(CFLAGS) -O1 -g -fsanitize=thread $(TSAN_OBJS) $(SQLITE_O) $(ARGON2_O) $(MBEDTLS_LIBS) $(MEDIA_LIBS) -fsanitize=thread -lresolv -lpthread -lm -o $@

# --- The release's compiler (CI) ----------------------------------------------
# The daemon's sources through the RELEASE's compiler, with the release's flags,
# stopping short of the link (the release links an ONNX Runtime built by the same
# compiler, which CI does not build): real objects, under build/relcc, since
# some warnings are raised only while generating code, one rule per object so
# that `make -j` compiles them all at once. A newer clang refuses things CI's
# accepts -- a declaration after a label, a C23 extension -- and the release is
# the wrong place to learn it. `RELEASE_CC` is the release toolchain's compiler
# (zig cc against glibc 2.34, as `make RELEASE=1` uses through zig_cc.sh). The same compiler then goes over everything else CI builds for
# Linux -- the client app-core, its media, voice and call code, and the tests --
# so this one clang pass is the tree's second-compiler check as well: -Werror
# makes any warning clang raises and gcc does not fatal.
RELEASE_CC ?= $(ZIG) cc -target x86_64-linux-gnu.2.34
RELEASE_CC_TEST_SRC := $(TEST_SRC) $(CORE_SRC) $(MEDIA_SRC) $(VOICE_SRC) $(CALL_SRC) \
                       $(SDLTEXT_COMMON) $(THEME_SRC) $(TTSKIT_SRC) $(QR_TEST_SRC) $(OSN_TEST_SRC)
# The release compiler as a file, like $(CC_STAMP): a different one recompiles.
RELCC_DIR   := build/relcc
RELCC_STAMP := $(RELCC_DIR)/.cc
$(RELCC_STAMP): FORCE | build-dir
	@mkdir -p $(dir $@)
	@echo '$(RELEASE_CC)' | cmp -s - $@ || echo '$(RELEASE_CC)' > $@
RELCC_DAEMON_CC   = $(RELEASE_CC) $(CFLAGS) $(VERSION_DEF) $(INC) $(SQLITE_INC) $(TTS_FLAGS) $(STT_FLAGS) $(SUM_FLAGS)
RELCC_TEST_CC     = $(RELEASE_CC) $(CFLAGS) $(TEST_INC)
RELCC_DAEMON_OBJS := $(call oc_objs,$(RELCC_DIR)/daemon,$(SRC) $(TTS_SRC) $(STT_SRC) $(SUM_SRC))
RELCC_TEST_OBJS   := $(call oc_objs,$(RELCC_DIR)/test,$(RELEASE_CC_TEST_SRC))
RELCC_VENDOR      := $(ZIG) $(MBEDTLS_A) $(TTS_DEPS) $(STT_DEPS) $(SUM_DEPS) $(LIBVPX_A) $(OPUS_A) $(SPEEXDSP_A)
$(eval $(call oc_obj_rules,$(RELCC_DIR)/daemon,RELCC_DAEMON_CC,$(RELCC_STAMP) $(FEATURES_STAMP) $(BUILD_ID_STAMP),$(RELCC_VENDOR),@))
$(eval $(call oc_obj_rules,$(RELCC_DIR)/test,RELCC_TEST_CC,$(RELCC_STAMP),$(RELCC_VENDOR),@))
-include $(RELCC_DAEMON_OBJS:.o=.d) $(RELCC_TEST_OBJS:.o=.d)
check-release-cc: $(RELCC_DAEMON_OBJS) $(RELCC_TEST_OBJS)
	@echo "check-release-cc: $(words $(SRC) $(TTS_SRC) $(STT_SRC) $(SUM_SRC) $(RELEASE_CC_TEST_SRC)) sources clean"

# --- Tool binaries, built by path ---------------------------------------------
# The e2e client the full suite drives against a natively-run daemon
# (scripts/test/full/e2e.sh, docs/TESTING.md §3); links only the shared wire
# modules (no daemon internals).
# Every tool signs in as a person does, through the daemon's pages (tests/testsignin.c).
build/e2e_client: tests/e2e_client.c tests/testsignin.c $(SHARED_SRC) $(wildcard shared/*.h tests/testsignin.h) $(MBEDTLS_A) | build-dir
	$(CC) $(CFLAGS) -O0 -g -Ishared -Itests -I$(MBEDTLS_INC) \
	    tests/e2e_client.c tests/testsignin.c $(SHARED_SRC) $(MBEDTLS_LIBS) -o $@
# The fuzzing tier (docs/TESTING.md, "Fuzzing"): one libFuzzer binary per
# harness in FUZZ_HARNESSES (mk/sources.mk), build/fuzz/<harness>, from
# tests/fuzz/<harness>.c, under ASan and UBSan. The host's clang, since zig's
# has no libFuzzer runtime: a prerequisite for building a harness only, never
# for the fast build or suite.
# By path when it is in the usual places: make puts the pinned llvm-mingw first
# on PATH (mk/toolchains.mk), and its clang has no Linux sanitizer runtimes.
FUZZ_CC  ?= $(firstword $(wildcard /usr/bin/clang /usr/local/bin/clang) clang)
FUZZ_LIB := tests/fuzz/decoders.c shared/protocol.c shared/framebuf.c
# What each harness links beyond FUZZ_LIB: the parser it fuzzes, and what that
# needs. Every harness links mbedTLS; jsmn's implementation is daemon/jwt.c's.
FUZZ_SRC_http     := daemon/http.c third_party/picohttpparser/picohttpparser.c daemon/jwt.c daemon/json.c
FUZZ_SRC_ws       := $(FUZZ_SRC_http)
FUZZ_SRC_proxy    := daemon/proxyproto.c
FUZZ_SRC_json     := daemon/json.c daemon/jwt.c
FUZZ_SRC_cbor     := daemon/cbor.c
FUZZ_SRC_jwt      := daemon/jwt.c daemon/json.c
FUZZ_SRC_webauthn := daemon/webauthn.c daemon/cbor.c daemon/json.c daemon/jwt.c
FUZZ_SRC_idtoken  := daemon/idtoken.c daemon/json.c daemon/jwt.c
FUZZ_ALL_SRC := $(sort $(foreach h,$(FUZZ_HARNESSES),$(FUZZ_SRC_$(h))))
build/fuzz/%: tests/fuzz/main.c tests/fuzz/%.c $(FUZZ_LIB) $(FUZZ_ALL_SRC) $(MBEDTLS_A) \
              $(wildcard shared/*.h daemon/*.h tests/fuzz/*.h) | build-dir
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(CFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	    -Ishared -Idaemon -Itests -Ithird_party/jsmn -Ithird_party/picohttpparser -I$(MBEDTLS_INC) \
	    -DOC_FUZZ_TARGET=oc_fuzz_$* tests/fuzz/main.c tests/fuzz/$*.c \
	    $(filter-out tests/fuzz/$*.c,$(FUZZ_LIB)) $(FUZZ_SRC_$*) $(MBEDTLS_LIBS) -o $@
# Writes one valid frame per message type, and a few streams of them, as the
# harnesses' seed corpora (tests/fuzz/corpus/): build/fuzz/seeds <dir>.
build/fuzz/seeds: tests/fuzz/seeds.c shared/protocol.c $(wildcard shared/*.h) | build-dir
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O0 -g -Ishared tests/fuzz/seeds.c shared/protocol.c -o $@
# Flexible black-box client for the local federated demo (register a device token /
# send a message against a running daemon). Same shared-only linkage as e2e_client.
build/demo_client: tests/demo_client.c tests/testsignin.c $(SHARED_SRC) $(wildcard shared/*.h tests/testsignin.h) $(MBEDTLS_A) | build-dir
	$(CC) $(CFLAGS) -O0 -g -Ishared -Itests -I$(MBEDTLS_INC) \
	    tests/demo_client.c tests/testsignin.c $(SHARED_SRC) $(MBEDTLS_LIBS) -o $@
# Capacity benchmark load client (REQ-210/211); driven by tools/bench.sh.
build/bench_load: tests/bench_load.c tests/testsignin.c $(SHARED_SRC) $(wildcard shared/*.h tests/testsignin.h) $(MBEDTLS_A) | build-dir
	$(CC) $(CFLAGS) -O2 -Ishared -Itests -I$(MBEDTLS_INC) \
	    tests/bench_load.c tests/testsignin.c $(SHARED_SRC) $(MBEDTLS_LIBS) -lpthread -o $@
# The sign-in helper as a command, for the shell scripts: prints the token and
# the verifier a client presents on AUTH, and presents one (`present`) for a
# script that played the browser itself (scripts/test/full/web.sh).
build/signin: tests/testsignin_main.c tests/testsignin.c $(SHARED_SRC) $(wildcard shared/*.h tests/testsignin.h) $(MBEDTLS_A) | build-dir
	$(CC) $(CFLAGS) -O0 -g -Ishared -Itests -I$(MBEDTLS_INC) \
	    tests/testsignin_main.c tests/testsignin.c $(SHARED_SRC) $(MBEDTLS_LIBS) -o $@
# Manual S3 smoke test (tests/manual/s3_smoke.c) -- in neither suite.
# Needs live S3 credentials in the environment; see the header of that file.
build/s3_smoke: tests/manual/s3_smoke.c daemon/blobstore.c daemon/blob_cache.c daemon/blob_s3.c daemon/sigv4.c daemon/config.c daemon/config_table.c daemon/proxyproto.c shared/url.c shared/tls.c third_party/ca-roots/ca_roots.c $(MBEDTLS_A) | build-dir
	$(CC) $(CFLAGS) $(INC) tests/manual/s3_smoke.c daemon/blobstore.c daemon/blob_cache.c daemon/blob_s3.c daemon/sigv4.c \
	    daemon/config.c daemon/config_table.c daemon/proxyproto.c shared/url.c shared/tls.c \
	    third_party/ca-roots/ca_roots.c $(MBEDTLS_LIBS) -lpthread -o $@
	@echo "built $@ -- needs OPENCHIME_S3 to run"
# ttskit's data tool (docs/TTSKIT.md).
build/tts_pack: $(TTSKIT_SRC) ttskit/tts_pack.c $(wildcard ttskit/*.h) | build-dir
	$(CC) $(CFLAGS) $(TTSKIT_INC) $(TTSKIT_SRC) ttskit/tts_pack.c -lm -o $@
