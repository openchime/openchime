# The daemon: openchimed, with whichever features the switches turn on
# (Makefile, "Features"), and the data directories beside it.

# --- Read-aloud (ARCH-111) ----------------------------------------------------
# ttskit and ONNX Runtime are compiled in; the voice model, its voices and the
# pronunciation data are DATA, assembled into tts/ beside the binary with a
# manifest the daemon checks at startup. Absent or mismatched data turns
# read-aloud off rather than stopping the daemon. tts/ beside openchimed is the
# same layout the tarball ships, so a source build finds its data the way an
# extracted release does.
# What `make test` links of read-aloud whatever TTS is: the render path and worker
# (driven by a stub engine) and the tokenizer -- everything but the model.
TTS_TEST_SRC := daemon/tts_render.c daemon/tts_worker.c daemon/tts_kitten_tokens.c
# ONNX Runtime and llama.cpp are C++: their standard library and runtime support
# are linked in statically, not required of the host. That is GCC's libstdc++ by
# default; the release, which compiles everything with zig, passes
# TTS_CXXLIB=-lc++ for zig's bundled libc++ (the library they were compiled
# against there). One of ONNX Runtime's assembly kernels lacks the GNU-stack
# note, which would otherwise mark the whole daemon's stack executable; the
# stack stays non-executable.
TTS_CXXLIB ?= -static-libstdc++ -static-libgcc -Wl,-Bstatic -lstdc++ -Wl,-Bdynamic -ldl
ifeq ($(TTS),1)
TTS_SRC   := daemon/tts.c daemon/tts_render.c daemon/tts_worker.c daemon/tts_kitten.c daemon/tts_kitten_tokens.c $(TTSKIT_SRC)
TTS_DEPS  := $(ORT_A) $(OPUS_A) $(wildcard ttskit/*.h)
TTS_DATA  := tts/manifest
TTS_FLAGS := -DOC_TTS $(TTSKIT_INC) -I$(ORT_DIR)/include -I$(OPUS_DIR)/include
TTS_LIBS  := $(OPUS_A)
endif

# --- Voice input (ARCH-112) ---------------------------------------------------
# The recognizer's code runs on the same ONNX Runtime; Moonshine's files are
# assembled into stt/ beside the binary with a manifest the daemon checks at
# startup. Absent or mismatched data turns voice input off, and read-aloud with
# it untouched.
# What `make test` links of voice input whatever STT is: the worker (driven by a
# stub engine) and the tokenizer -- everything but the model.
QR_TEST_SRC  := tuikit/tk_qr.c   # qrcodegen is the daemon's own now (APP_SRC)
OSN_TEST_SRC := client/shared/osnotify_null.c client/shared/feedback.c
STT_TEST_SRC := daemon/stt_worker.c daemon/stt_tokens.c daemon/stt_mentions.c
ifeq ($(STT),1)
STT_SRC   := daemon/stt.c daemon/stt_moonshine.c daemon/stt_worker.c daemon/stt_tokens.c daemon/stt_mentions.c
STT_DEPS  := $(ORT_A)
STT_DATA  := stt/manifest
STT_FLAGS := -DOC_STT -I$(ORT_DIR)/include
endif

# --- Summaries (ARCH-116) -----------------------------------------------------
# llama.cpp runs the summary model inside openchimed on one thread. Everything
# else of summaries -- the pieces, prompts, checks, storage and worker -- is
# plain C in DAEMON_SRC, so `make test` drives it with a stub model. Without
# SUM, summaries are never available locally (OPENCHIME_SUMMARY=cloud still is).
ifeq ($(SUM),1)
SUM_SRC   := daemon/sum_llama.c
SUM_DEPS  := $(LLAMA_A)
SUM_FLAGS := -DOC_SUM -I$(LLAMA_DIR)/include
SUM_LIBS  := $(LLAMA_A)
SUM_CXX   := $(if $(filter 1,$(TTS) $(STT)),,$(TTS_CXXLIB) -lm)
endif

# ONNX Runtime and its C++ runtime, once, for whichever speech features are in.
ifneq ($(filter 1,$(TTS) $(STT)),)
ORT_LIBS := $(ORT_A) $(TTS_CXXLIB) -lm -Wl,--gc-sections -Wl,-z,noexecstack
endif

$(BIN): $(SRC) $(TTS_SRC) $(STT_SRC) $(SUM_SRC) $(MBEDTLS_A) $(HDRS) $(TTS_DEPS) $(STT_DEPS) $(SUM_DEPS) $(SQLITE_O) $(CC_STAMP) $(FEATURES) $(TOOLCHAIN_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(VERSION_DEF) $(INC) $(SQLITE_INC) $(TTS_FLAGS) $(STT_FLAGS) $(SUM_FLAGS) -o $@ $(SRC) $(TTS_SRC) $(STT_SRC) $(SUM_SRC) $(SQLITE_O) $(MBEDTLS_LIBS) $(TTS_LIBS) $(SUM_LIBS) $(ORT_LIBS) $(SUM_CXX) $(LDFLAGS)

# The data directories, and their manifests written by the daemon just built --
# so the version they name is exactly the version compiled in. Rebuilt whenever
# the daemon is, which is whenever that version could have changed. A
# cross-built daemon cannot run here, so the HOST's daemon, built from the same
# sources with the same features, writes them: the data and the manifest are
# the same for every architecture.
ifeq ($(ARCH_SUFFIX),)
MANIFEST_BIN := $(BIN)
else
MANIFEST_BIN := build/manifest-host/openchimed
$(MANIFEST_BIN): FORCE
	@$(MAKE) --no-print-directory ARCH=$(HOST_ARCH) RELEASE=0 CC=cc OC_ARCH= openchimed
	@mkdir -p $(dir $@) && cp openchimed $@
endif
tts/manifest: $(MANIFEST_BIN) $(KITTEN) build/kitten/voices.npz ttskit/data/en-US/lexicon.bin ttskit/data/en-US/guesses.bin
	mkdir -p tts/en-US
	cp $(KITTEN) build/kitten/voices.npz tts/
	cp ttskit/data/en-US/lexicon.bin ttskit/data/en-US/guesses.bin tts/en-US/
	$(abspath $(MANIFEST_BIN)) --tts-manifest tts
stt/manifest: $(MANIFEST_BIN) build/moonshine/.done
	mkdir -p stt
	cp $(addprefix build/moonshine/,frontend.model.ort frontend.weights.ort encoder.ort adapter.ort cross_kv.ort decoder_kv.ort tokenizer.bin streaming_config.json) stt/
	$(abspath $(MANIFEST_BIN)) --stt-manifest stt
