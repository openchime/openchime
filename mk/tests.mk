# The tests (docs/TESTING.md), CI's split of them, the release-compiler pass,
# and the tool binaries a person builds by path.

# Source checks run before the suite, because a C test cannot inspect the text
# of the tree: no two message types share an opcode (the wire contract's one
# static invariant); no file cites an issue by number (CONTRIBUTING.md: a
# commit cites the issue it closes; a file says the thing instead, because a
# number in a comment rots silently the moment the issue does); and the
# Makefile's typed targets are the ones its header names; the configuration
# registry is the one source of every OPENCHIME_* name, CONFIG.md's reference
# and the packaged environment file (config.sh, which asks the daemon, so
# `test` builds it first); nothing that ships is a box in development mode
# (release_lint.sh); and the tests wait on outcomes, compare markers rather
# than a person's words, share no paths, and the workflows pipe nothing into
# grep -q under pipefail (tests_lint.sh, docs/TESTING.md "Brittle tests").
SOURCE_CHECKS := scripts/check/opcodes.sh shared/protocol.h && scripts/check/refs.sh && scripts/check/make_targets.sh && scripts/check/config.sh && scripts/check/release_lint.sh && scripts/check/tests_lint.sh

# Unit + in-process integration tests, one binary (docs/TESTING.md §2). Built
# -O0 -g; a non-zero exit fails the build and CI. The suites run side by side,
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
TEST_DEPS := $(TEST_ALL_SRC) $(VOICE_HDRS) $(CALL_HDRS) $(HDRS) $(MEDIA_HDRS) \
             $(wildcard tests/*.h client/core/*.h sdltext/*.h ttskit/*.h daemon/tts_*.h daemon/stt_*.h client/gui/app/theme.h) \
             $(MBEDTLS_A) $(LIBVPX_A) $(OPUS_A) $(SPEEXDSP_A) $(SQLITE_O) $(ARGON2_O) $(CC_STAMP)
test: $(TEST_BIN) $(BIN)
	@$(SOURCE_CHECKS)
	OC_TEST_JOBS="$${OC_TEST_JOBS:-$(TEST_JOBS)}" ./$(TEST_BIN)
$(TEST_BIN): $(TEST_DEPS) | build
	$(CC) $(CFLAGS) -O0 -g $(TEST_INC) $(TEST_ALL_SRC) $(SQLITE_O) $(ARGON2_O) $(MBEDTLS_LIBS) $(MEDIA_LIBS) -lresolv -lpthread -lm -o $@

# --- CI's split (docs/TESTING.md §2.4) ----------------------------------------
# The suites whose code runs threads -- the loop and its I/O threads, the
# writer and readers, the worker pools, the client, the recorder and player --
# run under ThreadSanitizer and the rest plainly, so every suite runs once
# there, never twice. A suite with no thread cannot race, and instrumenting it
# only makes it slower. test-ci refuses outside CI, before its long build. CI
# runs its halves in parallel jobs: PART=plain or PART=threads; unset, both.
comma := ,
TSAN_LIST := run_netloop_tests run_client_core_tests run_dbwriter_tests run_ioloop_tests \
             run_authpool_tests run_xferpool_tests run_slow_blob_tests run_storage_tests \
             run_blob_s3_tests run_media_tests run_video_media_tests run_tts_worker_tests \
             run_stt_tests run_push_tests run_invite_mail_tests run_enroll_tests \
             run_tls_tests run_signin_tests run_acme_tests
TSAN_SUITES := $(subst $(eval) ,$(comma),$(strip $(TSAN_LIST)))
# The same test binary under ThreadSanitizer. The vendored libraries are not
# instrumented, so a race inside one is not seen; one between our threads is.
# Run with address randomization off (setarch -R): ThreadSanitizer maps its
# shadow memory at fixed addresses, and the randomization of recent kernels
# places the binary where that shadow has to go ("unexpected memory mapping").
TSAN_BIN := build/tests-tsan
ci-only:
	@if [ -z "$$CI" ]; then echo "make test-ci runs in CI only; locally, run make test"; exit 1; fi
test-ci: ci-only
	@$(MAKE) --no-print-directory $(if $(filter plain,$(PART)),test-ci-plain,$(if $(filter threads,$(PART)),test-ci-threads,test-ci-plain test-ci-threads))
test-ci-plain: ci-only $(TEST_BIN) $(BIN)
	@$(SOURCE_CHECKS)
	OC_TEST_JOBS="$${OC_TEST_JOBS:-$(TEST_JOBS)}" OC_TEST_EXCEPT="$(TSAN_SUITES)" ./$(TEST_BIN)
test-ci-threads: ci-only $(TSAN_BIN)
	OC_TEST_ONLY="$${OC_TEST_ONLY:-$(TSAN_SUITES)}" \
	TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 suppressions=$(CURDIR)/tests/tsan.supp" setarch $$(uname -m) -R ./$(TSAN_BIN)
$(TSAN_BIN): $(TEST_DEPS) | build
	$(CC) $(CFLAGS) -O1 -g -fsanitize=thread $(TEST_INC) $(TEST_ALL_SRC) $(SQLITE_O) $(ARGON2_O) $(MBEDTLS_LIBS) $(MEDIA_LIBS) -fsanitize=thread -lresolv -lpthread -lm -o $@

# --- The release's compiler (CI) ----------------------------------------------
# The daemon's sources through the RELEASE's compiler, with the release's flags,
# stopping short of the link (the release links an ONNX Runtime built by the same
# compiler, which CI does not build). A newer clang refuses things CI's accepts --
# a declaration after a label, a C23 extension, broke a release this way -- and
# the release is the wrong place to learn it. `RELEASE_CC` is what release.yml
# compiles with. The same compiler then goes over everything else CI builds for
# Linux -- the client app-core, its media, voice and call code, and the tests --
# so this one clang pass is the tree's second-compiler check as well: -Werror
# makes any warning clang raises and gcc does not fatal.
RELEASE_CC ?= $(ZIG) cc -target x86_64-linux-gnu.2.34
RELEASE_CC_TEST_SRC := $(TEST_SRC) $(CORE_SRC) $(MEDIA_SRC) $(VOICE_SRC) $(CALL_SRC) \
                       $(SDLTEXT_COMMON) $(THEME_SRC) $(TTSKIT_SRC) $(QR_TEST_SRC) $(OSN_TEST_SRC)
check-release-cc: $(ZIG) $(MBEDTLS_A) $(TTS_DEPS) $(STT_DEPS) $(SUM_DEPS) $(LIBVPX_A) $(OPUS_A) $(SPEEXDSP_A) $(WEBAPP_RES) $(PWLIST_RES)
	@for f in $(SRC) $(TTS_SRC) $(STT_SRC) $(SUM_SRC); do \
	  $(RELEASE_CC) $(CFLAGS) $(VERSION_DEF) $(INC) $(SQLITE_INC) $(TTS_FLAGS) $(STT_FLAGS) $(SUM_FLAGS) -c -o /dev/null $$f || exit 1; \
	done
	@for f in $(RELEASE_CC_TEST_SRC); do \
	  $(RELEASE_CC) $(CFLAGS) $(TEST_INC) -c -o /dev/null $$f || exit 1; \
	done; echo "check-release-cc: $(words $(SRC) $(TTS_SRC) $(STT_SRC) $(SUM_SRC) $(RELEASE_CC_TEST_SRC)) sources clean"

# --- Tool binaries, built by path ---------------------------------------------
# The e2e client CI's build job drives against a natively-run daemon (docs/
# TESTING.md §3); links only the shared wire modules (no daemon internals).
# Every tool signs in as a person does, through the daemon's pages (tests/testsignin.c).
build/e2e_client: tests/e2e_client.c tests/testsignin.c $(SHARED_SRC) $(wildcard shared/*.h tests/testsignin.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O0 -g -Ishared -Itests -I$(MBEDTLS_INC) \
	    tests/e2e_client.c tests/testsignin.c $(SHARED_SRC) $(MBEDTLS_LIBS) -o $@
# Flexible black-box client for the local federated demo (register a device token /
# send a message against a running daemon). Same shared-only linkage as e2e_client.
build/demo_client: tests/demo_client.c tests/testsignin.c $(SHARED_SRC) $(wildcard shared/*.h tests/testsignin.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O0 -g -Ishared -Itests -I$(MBEDTLS_INC) \
	    tests/demo_client.c tests/testsignin.c $(SHARED_SRC) $(MBEDTLS_LIBS) -o $@
# Capacity benchmark load client (REQ-210/211); driven by scripts/bench.sh.
build/bench_load: tests/bench_load.c tests/testsignin.c $(SHARED_SRC) $(wildcard shared/*.h tests/testsignin.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O2 -Ishared -Itests -I$(MBEDTLS_INC) \
	    tests/bench_load.c tests/testsignin.c $(SHARED_SRC) $(MBEDTLS_LIBS) -lpthread -o $@
# Event-loop load harness: the daemon in-process, driven by loopback clients,
# printing the net loop's turn percentiles beside client latency. Optimised like
# the daemon, so the numbers are the daemon's; not part of `make test`.
build/bench_loop: tests/bench_loop.c tests/testsignin.c $(APP_SRC) $(HDRS) $(MBEDTLS_A) $(SQLITE_O) $(ARGON2_O) | build
	$(CC) $(CFLAGS) $(INC) $(SQLITE_INC) -Itests \
	    tests/bench_loop.c tests/testsignin.c $(APP_SRC) $(SQLITE_O) $(ARGON2_O) $(MBEDTLS_LIBS) -ldl -lpthread -lm -o $@
# The sign-in helper as a command, for the shell scripts: prints the token and
# the verifier a client presents on AUTH, and presents one (`present`) for a
# script that played the browser itself (scripts/web/pages.sh).
build/signin: tests/testsignin_main.c tests/testsignin.c $(SHARED_SRC) $(wildcard shared/*.h tests/testsignin.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O0 -g -Ishared -Itests -I$(MBEDTLS_INC) \
	    tests/testsignin_main.c tests/testsignin.c $(SHARED_SRC) $(MBEDTLS_LIBS) -o $@
# Manual S3 smoke test (tests/manual/s3_smoke.c) -- NOT part of `make test`.
# Needs live S3 credentials in the environment; see the header of that file.
build/s3_smoke: tests/manual/s3_smoke.c daemon/blobstore.c daemon/blob_s3.c daemon/sigv4.c daemon/config.c daemon/config_table.c daemon/proxyproto.c shared/url.c shared/tls.c third_party/ca-roots/ca_roots.c $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) $(INC) tests/manual/s3_smoke.c daemon/blobstore.c daemon/blob_s3.c daemon/sigv4.c \
	    daemon/config.c daemon/config_table.c daemon/proxyproto.c shared/url.c shared/tls.c \
	    third_party/ca-roots/ca_roots.c $(MBEDTLS_LIBS) -lpthread -o $@
	@echo "built $@ -- needs OPENCHIME_S3 to run"
# Standalone harness exercising every tuikit widget (ARCH-83) -- no core, no
# daemon, no TLS. The toolbox's own smoke test.
build/tuikit-demo: $(TUIKIT_SRC) tuikit/demo.c $(UTF8PROC) $(wildcard tuikit/*.h) | build
	$(CC) $(CFLAGS) -Wno-unused-result $(TUIKIT_INC) -Ithird_party/termbox2 -Ithird_party/utf8proc \
	    $(TUIKIT_SRC) tuikit/demo.c $(UTF8PROC) -o $@
# ttskit's data tool (docs/TTSKIT.md).
build/tts_pack: $(TTSKIT_SRC) ttskit/tts_pack.c $(wildcard ttskit/*.h) | build
	$(CC) $(CFLAGS) $(TTSKIT_INC) $(TTSKIT_SRC) ttskit/tts_pack.c -lm -o $@
