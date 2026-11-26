# The test binaries and the fast suite (docs/details/TESTING.md), the release-compiler
# pass, and the tool binaries a person builds by path. Every test is a script
# in scripts/test/fast/ or scripts/test/full/; a target builds what the scripts
# need and calls them, and the workflows call only the targets.

# Unit + in-process integration tests, one binary (docs/details/TESTING.md §2). Built
# -O0 -g; a non-zero exit fails the run. The suites run one after another, in
# one process (scripts/test/fast/suites.sh sets OC_TEST_JOBS=1; docs/details/TESTING.md,
# "One build at a time"). The binary
# always carries read-aloud's and voice input's test paths (stub engines) and
# never needs Emscripten, whatever the switches say.
TEST_INC  := $(INC) $(PREVIEW_INC) $(SQLITE_INC) $(CORE_INC) $(MEDIA_INC) $(VOICE_INC) $(CALL_INC) $(TTSKIT_INC) $(STTKIT_INC) $(SUMKIT_INC) -DOC_TTS -DOC_STT $(EXPORT_TEST_INC) \
             -Itests -Iclient/gui/app -Iclient/gui/platform -Iclient/shared -Ituikit -Ithird_party/qrcodegen
# Sorted: the daemon and the text layout both carry utf8proc, compiled once.
TEST_ALL_SRC := $(sort $(TEST_SRC) $(APP_SRC) $(CORE_SRC) $(MEDIA_SRC) $(VOICE_SRC) $(CALL_SRC) $(SDLTEXT_COMMON) $(THEME_SRC) $(TYPEFACE_SRC) $(MOTION_SRC) \
                $(TTSKIT_SRC) $(NNKIT_SRC) $(SUMKIT_SRC) $(TTS_TEST_SRC) $(STT_TEST_SRC) $(QR_TEST_SRC) $(OSN_TEST_SRC) $(EXPORT_TEST_SRC) $(CALLREC_TEST_SRC))
# One object per source, in a tree of its own (mk/sources.mk, "Objects"),
# compiled with the flags the binary always had; the link takes them in the
# sorted order above, then SQLite and Argon2 (the daemon's own objects) and the
# vendored libraries, which only have to exist before anything compiles.
TEST_CC     = $(CC) $(CFLAGS) -O0 -g $(TEST_INC)
TEST_OBJS   := $(call oc_objs,$(OBJ_DIR)/test,$(TEST_ALL_SRC))
TEST_VENDOR := $(MBEDTLS_A) $(PREVIEW_A) $(LIBVPX_A) $(OPUS_A) $(SPEEXDSP_A) $(EXPORT_TEST_LIBS)
$(eval $(call oc_obj_rules,$(OBJ_DIR)/test,TEST_CC,$(CC_STAMP),$(TEST_VENDOR)))
-include $(TEST_OBJS:.o=.d)
# --- The fast suite (docs/details/TESTING.md) -----------------------------------------
# `make test` (Makefile) runs this in a make of the fast build's own: the fast
# build and the test binary, then the source checks, actionlint on the
# workflows and the suites SUITES[] marks FAST, and on a pass the tree it
# passed on, recorded for the pre-push hook (scripts/test/lib/tested_tree.sh).
# The full suite is mk/full.mk.
fast-test: fast-build $(TEST_BIN)
	scripts/test/fast/source.sh
	scripts/test/fast/actionlint.sh
	scripts/test/fast/suites.sh
	@scripts/test/lib/tested_tree.sh record

TSAN_BIN := build/tests-tsan
# pthread_create is wrapped in the plain test binary, so the runner sees which
# suites start threads and fails one SUITES[] does not mark THREADS
# (tests/main.c).
$(TEST_BIN): $(TEST_OBJS) $(SQLITE_O) $(ARGON2_O) $(TEST_VENDOR) $(CC_STAMP) | build-dir
	$(CC) $(CFLAGS) -O0 -g $(TEST_OBJS) $(SQLITE_O) $(ARGON2_O) $(MBEDTLS_LIBS) $(PREVIEW_LIBS) $(EXPORT_TEST_LIBS) $(MEDIA_LIBS) -Wl,--wrap=pthread_create -lresolv -lpthread -lm -o $@
# The same test binary under ThreadSanitizer (scripts/test/full/tsan.sh runs
# it). The vendored libraries are not instrumented.
TSAN_CC   = $(CC) $(CFLAGS) -O1 -g -fsanitize=thread $(TEST_INC)
TSAN_OBJS := $(call oc_objs,$(OBJ_DIR)/tsan,$(TEST_ALL_SRC))
$(eval $(call oc_obj_rules,$(OBJ_DIR)/tsan,TSAN_CC,$(CC_STAMP),$(TEST_VENDOR)))
-include $(TSAN_OBJS:.o=.d)
$(TSAN_BIN): $(TSAN_OBJS) $(SQLITE_O) $(ARGON2_O) $(TEST_VENDOR) $(CC_STAMP) | build-dir
	$(CC) $(CFLAGS) -O1 -g -fsanitize=thread $(TSAN_OBJS) $(SQLITE_O) $(ARGON2_O) $(MBEDTLS_LIBS) $(PREVIEW_LIBS) $(EXPORT_TEST_LIBS) $(MEDIA_LIBS) -fsanitize=thread -lresolv -lpthread -lm -o $@

# --- Test programs built by path ------------------------------------------------
# sdltext's one test program (sdltext/st_test.c), here on the FreeType backend;
# mk/windows.mk builds the same program against DirectWrite. Its exit code is
# its failure count. It needs FreeType, HarfBuzz and fontconfig (FT_PKGS), so
# neither `make build` nor `make test` builds it.
SDLTEXT_TEST_SRC := sdltext/st_test.c $(SDLTEXT_FT)
build/sdltext_test: $(SDLTEXT_TEST_SRC) $(wildcard sdltext/*.h) | build-dir
	$(call oc_need_pkgs,$@,$(FT_PKGS))
	$(CC) $(CFLAGS) -std=c11 -O0 -g $(SDLTEXT_INC) -DUTF8PROC_STATIC $(FT_CFLAGS) $(SDLTEXT_TEST_SRC) $(FT_LIBS) -lm -o $@
# The Linux GUI's test program (tests/linux_gui_test.c): the platform, the
# AT-SPI provider, notifications and portal capture, against stand-in D-Bus
# services it serves itself on the session bus `dbus-run-session` gives it. Its
# exit code is its failure count. It links the GUI's own objects (mk/linux.mk),
# compiled as the GUI compiles them, and none of the application layer, so it
# needs the GUI's development packages (LINUX_GUI_PKGS), and neither `make
# build` nor `make test` builds it. Its own object comes first, so the
# packages are checked, naming any missing, before anything else is built.
# malloc is wrapped so the image checks can see what a decode allocates.
LINUX_GUI_TEST_SRC  := client/gui/platform/linux/plat_linux.c client/gui/platform/linux/a11y_atspi.c \
                       client/shared/osnotify_linux.c client/shared/icons.c $(THEME_SRC) $(GFX_SRC) $(SDLTEXT_FT) \
                       client/core/media/cap_portal.c client/core/media/capture.c client/core/media/i420.c \
                       client/core/env.c
LINUX_GUI_TEST_OBJS := $(OBJ_DIR)/linux-gui-test/tests/linux_gui_test.o \
                       $(call oc_objs,$(OBJ_DIR)/linux-gui,$(LINUX_GUI_TEST_SRC))
LINUX_GUI_TEST_PKGS_OK := build/.linux_gui_test_pkgs
$(LINUX_GUI_TEST_PKGS_OK): FORCE
	$(call oc_need_pkgs,build/linux_gui_test,$(LINUX_GUI_PKGS))
LINUX_GUI_TEST_CC = $(LINUX_GUI_CC) -Itests
$(eval $(call oc_obj_rules,$(OBJ_DIR)/linux-gui-test,LINUX_GUI_TEST_CC,$(CC_STAMP),$(LINUX_GUI_TEST_PKGS_OK) $(LINUX_GUI_A)))
-include $(OBJ_DIR)/linux-gui-test/tests/linux_gui_test.d
build/linux_gui_test: $(LINUX_GUI_TEST_OBJS) $(PREVIEW_A) $(SDL3_LINUX_LIB) | build-dir
	$(CC) $(CFLAGS) -std=c11 -g $(LINUX_GUI_TEST_OBJS) -Wl,--wrap=malloc $(PREVIEW_LIBS) $(SDL3_LINUX_LIB) \
	    $(LINUX_GUI_PKG_LIBS) -lpthread -ldl -lm -o $@

# --- Tool binaries, built by path ---------------------------------------------
# The e2e client the full suite drives against a natively-run daemon
# (scripts/test/full/e2e.sh, docs/details/TESTING.md §3); links only the shared wire
# modules (no daemon internals).
# Every tool signs in as a person does, through the daemon's pages (tests/testsignin.c).
build/e2e_client: tests/e2e_client.c tests/testsignin.c $(SHARED_SRC) $(wildcard shared/*.h tests/testsignin.h) $(MBEDTLS_A) | build-dir
	$(CC) $(CFLAGS) -O0 -g -Ishared -Itests -I$(MBEDTLS_INC) \
	    tests/e2e_client.c tests/testsignin.c $(SHARED_SRC) $(MBEDTLS_LIBS) -o $@
# The fuzzing tier (docs/details/TESTING.md, "Fuzzing"): one libFuzzer binary per
# harness in FUZZ_HARNESSES (mk/sources.mk), build/fuzz/<harness>, from
# tests/fuzz/<harness>.c, under ASan and UBSan. The host's clang, for its
# libFuzzer runtime: a prerequisite for building a harness only, never
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
# The image harness links the preview engine and its three decoders, built as
# the daemon's are: only daemon/preview.c is instrumented for coverage.
FUZZ_SRC_image    := daemon/preview.c
FUZZ_LIBS_image   := $(PREVIEW_LIBS)
FUZZ_ALL_SRC := $(sort $(foreach h,$(FUZZ_HARNESSES),$(FUZZ_SRC_$(h))))
build/fuzz/%: tests/fuzz/main.c tests/fuzz/%.c $(FUZZ_LIB) $(FUZZ_ALL_SRC) $(MBEDTLS_A) $(PREVIEW_A) \
              $(wildcard shared/*.h daemon/*.h tests/fuzz/*.h) | build-dir
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(CFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	    -Ishared -Idaemon -Itests -Ithird_party/jsmn -Ithird_party/picohttpparser -I$(MBEDTLS_INC) $(PREVIEW_INC) \
	    -DOC_FUZZ_TARGET=oc_fuzz_$* tests/fuzz/main.c tests/fuzz/$*.c \
	    $(filter-out tests/fuzz/$*.c,$(FUZZ_LIB)) $(FUZZ_SRC_$*) $(MBEDTLS_LIBS) $(FUZZ_LIBS_$*) -lm -o $@
# Writes one valid frame per message type, and a few streams of them, as the
# harnesses' seed corpora (tests/fuzz/corpus/): build/fuzz/seeds <dir>.
build/fuzz/seeds: tests/fuzz/seeds.c shared/protocol.c $(wildcard shared/*.h) | build-dir
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O0 -g -Ishared tests/fuzz/seeds.c shared/protocol.c -o $@
# Flexible black-box client for the local services demo (register a device token /
# send a message against a running daemon). Same shared-only linkage as e2e_client.
build/demo_client: tests/demo_client.c tests/testsignin.c $(SHARED_SRC) $(wildcard shared/*.h tests/testsignin.h) $(MBEDTLS_A) | build-dir
	$(CC) $(CFLAGS) -O0 -g -Ishared -Itests -I$(MBEDTLS_INC) \
	    tests/demo_client.c tests/testsignin.c $(SHARED_SRC) $(MBEDTLS_LIBS) -o $@
# Capacity benchmark load client (REQ-210/211); driven by tools/bench.sh.
build/bench_load: tests/bench_load.c tests/testsignin.c $(SHARED_SRC) $(wildcard shared/*.h tests/testsignin.h) $(MBEDTLS_A) | build-dir
	$(CC) $(CFLAGS) -O2 -Ishared -Itests -I$(MBEDTLS_INC) \
	    tests/bench_load.c tests/testsignin.c $(SHARED_SRC) $(MBEDTLS_LIBS) -lpthread -o $@
# Manual S3 smoke test (tests/manual/s3_smoke.c) -- in neither suite.
# Needs live S3 credentials in the environment; see the header of that file.
build/s3_smoke: tests/manual/s3_smoke.c daemon/blobstore.c daemon/blob_cache.c daemon/blob_s3.c daemon/sigv4.c daemon/config.c daemon/config_table.c daemon/proxyproto.c shared/url.c shared/tls.c third_party/ca-roots/ca_roots.c $(MBEDTLS_A) | build-dir
	$(CC) $(CFLAGS) $(INC) tests/manual/s3_smoke.c daemon/blobstore.c daemon/blob_cache.c daemon/blob_s3.c daemon/sigv4.c \
	    daemon/config.c daemon/config_table.c daemon/proxyproto.c shared/url.c shared/tls.c \
	    third_party/ca-roots/ca_roots.c $(MBEDTLS_LIBS) -lpthread -o $@
	@echo "built $@ -- needs OPENCHIME_S3 to run"
# sttkit's check (docs/details/STTKIT.md): the recognizer against its references, timed.
STT_CHECK_SRC := $(STTKIT_SRC) $(NNKIT_SRC) sttkit/stt_check.c
build/stt_check: $(STT_CHECK_SRC) $(STTKIT_HDRS) $(NNKIT_HDRS) | build-dir
	$(CC) $(CFLAGS) $(KIT_CFLAGS) $(STTKIT_INC) $(STT_CHECK_SRC) -lm -o $@
# sumkit's check (docs/details/SUMKIT.md): the model against its references, its
# quantisation measured, timed.
SUM_CHECK_SRC := $(SUMKIT_SRC) $(NNKIT_SRC) $(UTF8PROC) sumkit/sum_check.c
build/sum_check: $(SUM_CHECK_SRC) $(SUMKIT_HDRS) $(NNKIT_HDRS) | build-dir
	$(CC) $(CFLAGS) $(KIT_CFLAGS) $(SUMKIT_INC) -Ithird_party/utf8proc -DUTF8PROC_STATIC $(SUM_CHECK_SRC) -lm -o $@
# ttskit's data tool (docs/details/TTSKIT.md). `say` turns IPA into the voice model's
# tokens with the daemon's token function, as the daemon does.
TTS_PACK_SRC := $(TTSKIT_SRC) $(NNKIT_SRC) ttskit/tts_pack.c daemon/tts_voice_tokens.c
build/tts_pack: $(TTS_PACK_SRC) $(wildcard ttskit/*.h) $(NNKIT_HDRS) daemon/tts_voice.h | build-dir
	$(CC) $(CFLAGS) $(KIT_CFLAGS) $(TTSKIT_INC) -Idaemon $(TTS_PACK_SRC) -lm -o $@
