# The daemon: openchimed, with whichever features the switches turn on
# (Makefile, "Features"), and the data directories beside it.

# --- Read-aloud (ARCH-111) ----------------------------------------------------
# ttskit is compiled in, the voice model run by it in plain C; the voice
# model's data (voice.bin, with its licence) and the pronunciation data are
# DATA, copied from ttskit/data/ into tts/ beside the binary with a manifest the
# daemon checks at startup. Nothing for read-aloud is downloaded or built
# outside the tree. Absent or mismatched data turns read-aloud off rather than
# stopping the daemon. tts/ beside openchimed is the same layout the tarball
# ships, so a source build finds its data the way an extracted release does.
# What `make test` links of read-aloud whatever TTS is: the render path and worker
# (driven by a stub engine), the tokenizer and the model's wrapper (ttskit is in
# every test build) -- everything but the engine table and --tts-say (tts.c).
TTS_TEST_SRC := daemon/tts_render.c daemon/tts_worker.c daemon/tts_voice.c daemon/tts_voice_tokens.c
ifeq ($(TTS),1)
TTS_SRC   := daemon/tts.c daemon/tts_render.c daemon/tts_worker.c daemon/tts_voice.c daemon/tts_voice_tokens.c $(TTSKIT_SRC) $(NNKIT_SRC)
TTS_DEPS  := $(OPUS_A) $(wildcard ttskit/*.h) $(NNKIT_HDRS)
TTS_DATA  := tts/manifest
TTS_FLAGS := -DOC_TTS $(TTSKIT_INC) -I$(OPUS_DIR)/include
TTS_LIBS  := $(OPUS_A)
endif

# --- Voice input (ARCH-112) ---------------------------------------------------
# sttkit is compiled in, the recognizer run by it in plain C; its data
# (recognizer.bin, with its licence) is DATA, copied from sttkit/data/ into stt/
# beside the binary with a manifest the daemon checks at startup. Nothing for
# voice input is downloaded or built outside the tree. Absent or mismatched data
# turns voice input off, and read-aloud with it untouched.
# What `make test` links of voice input whatever STT is: the worker (driven by a
# stub engine) and sttkit -- everything but the engine table and --stt-hear
# (stt.c).
QR_TEST_SRC  := tuikit/tk_qr.c   # qrcodegen is the daemon's own now (APP_SRC)
OSN_TEST_SRC := client/shared/osnotify_null.c client/shared/feedback.c
STT_TEST_SRC := daemon/stt_worker.c daemon/stt_mentions.c $(STTKIT_SRC)
ifeq ($(STT),1)
STT_SRC   := daemon/stt.c daemon/stt_worker.c daemon/stt_mentions.c $(STTKIT_SRC) $(if $(filter 1,$(TTS)),,$(NNKIT_SRC))
STT_DEPS  := $(STTKIT_HDRS) $(NNKIT_HDRS)
STT_DATA  := stt/manifest
STT_FLAGS := -DOC_STT $(STTKIT_INC)
endif

# --- Summaries (ARCH-116) -----------------------------------------------------
# sumkit runs the summary model inside openchimed on one thread. Everything
# else of summaries -- the pieces, prompts, checks, storage and worker -- is
# plain C in DAEMON_SRC, so `make test` drives it with a stub model. Without
# SUM, summaries are never available locally (OPENCHIME_SUMMARY=cloud:… still is).
ifeq ($(SUM),1)
SUM_SRC   := daemon/sum_sumkit.c
SUM_DEPS  := $(SUMKIT_HDRS)
SUM_FLAGS := -DOC_SUM
endif

# --- The export of a video message (ARCH-135) ---------------------------------
# The codecs a copy is made with: libvpx and libopus to decode the VP9 and Opus
# the clients record, libavc and LAME to encode H.264 and MP3, all C and all
# built here. The render, the worker and the frames are plain C in DAEMON_SRC,
# so `make test` drives them with a stub; only the engine is EXPORT's.
# Without EXPORT the daemon refuses every export, whatever OPENCHIME_H264_EXPORT says.
# The test binary links the engine whatever EXPORT is, for the export_media
# suite; it is the only one that asks for it (oc_export_codecs_real).
EXPORT_TEST_SRC  := daemon/export_engine.c
EXPORT_TEST_INC  := -I$(LIBAVC_DIR)/include -I$(LAME_DIR)/include
EXPORT_TEST_LIBS := $(LIBAVC_A) $(LAME_A)
ifeq ($(EXPORT),1)
EXPORT_SRC   := daemon/export_engine.c $(CODEC_DEC_SRC)
EXPORT_DEPS  := $(LIBAVC_A) $(LAME_A) $(LIBVPX_A) $(OPUS_A)
EXPORT_FLAGS := -DOC_EXPORT -I$(LIBAVC_DIR)/include -I$(LAME_DIR)/include -I$(LIBVPX_DIR)/include -I$(OPUS_DIR)/include
EXPORT_LIBS  := $(LIBAVC_A) $(LAME_A) $(LIBVPX_A) $(if $(filter 1,$(TTS)),,$(OPUS_A))
endif

# sumkit and the converter that writes its file are in every build, as the
# other models' converters are: `openchimed --summary-pack` makes summary.bin
# whether or not the engine runs it here. nnkit comes with read-aloud or voice
# input when either is built in, and on its own otherwise.
SUMKIT_DAEMON_SRC := $(SUMKIT_SRC) $(if $(filter 1,$(TTS) $(STT)),,$(NNKIT_SRC))

# One object per source (mk/sources.mk, "Objects"), compiled with exactly the
# flags the daemon always had, and linked in the sources' order. Every object
# is recompiled when the compiler, the features (which change the flags) or the
# build id (compiled into every object, as it always was) change; the vendored
# libraries only have to exist first, their headers being tracked by the .d
# files like ours. ttskit's and sttkit's headers are on the path in every build:
# the two models' data files are written in their formats (--voice-pack,
# daemon/voice_pack.c; --recognizer-pack, daemon/recognizer_pack.c) whether or
# not read-aloud or voice input is compiled in.
DAEMON_CC   = $(CC) $(CFLAGS) $(VERSION_DEF) $(INC) $(SQLITE_INC) $(TTSKIT_INC) $(STTKIT_INC) $(SUMKIT_INC) $(TTS_FLAGS) $(STT_FLAGS) $(SUM_FLAGS) $(EXPORT_FLAGS)
DAEMON_OBJS := $(call oc_objs,$(OBJ_DIR)/daemon,$(SRC) $(TTS_SRC) $(STT_SRC) $(SUMKIT_DAEMON_SRC) $(SUM_SRC) $(EXPORT_SRC))
$(eval $(call oc_obj_rules,$(OBJ_DIR)/daemon,DAEMON_CC,$(CC_STAMP) $(FEATURES_STAMP) $(BUILD_ID_STAMP),$(MBEDTLS_A) $(TTS_DEPS) $(STT_DEPS) $(SUM_DEPS) $(EXPORT_DEPS)))
-include $(DAEMON_OBJS:.o=.d)
$(BIN): $(DAEMON_OBJS) $(MBEDTLS_A) $(TTS_DEPS) $(STT_DEPS) $(SUM_DEPS) $(EXPORT_DEPS) $(SQLITE_O) $(ARGON2_O) $(CC_STAMP) $(FEATURES_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(DAEMON_OBJS) $(SQLITE_O) $(ARGON2_O) $(MBEDTLS_LIBS) $(TTS_LIBS) $(EXPORT_LIBS) $(LDFLAGS)

# The data directories, and their manifests written by the daemon just built --
# so the version they name is exactly the version compiled in. Rebuilt whenever
# the daemon is, which is whenever that version could have changed. A
# cross-built daemon cannot run here, so a HOST daemon from the same sources
# writes them. It is built with NO features: the manifest names each model's
# version, a constant of the sources, over hashes of the data, and neither
# needs an engine linked in -- so the arm64 build does not compile the host's
# summary engine for a binary that only writes two files. Its
# switches on its command line and nothing remembered (REMEMBER=0) and its own
# binary path, so a developer's remembered features and host daemon are left
# as they were; and its own generated sources, objects,
# compiler stamp, build-id stamp and SQLite and Argon2 objects, because under
# `make -j` it runs while this build does: sharing $(GEN_DIR) it would rewrite
# webapp_res.c without the web client, and the cross-built daemon would link
# that copy and ship without its client.
ifeq ($(ARCH_SUFFIX),)
MANIFEST_BIN := $(BIN)
else ifneq ($(MANIFEST_HOST_BIN),)
# MANIFEST_HOST_BIN: a daemon already built for this machine, which writes the
# manifests instead of a host daemon built for the purpose. The
# release-candidate's arm64 step passes the amd64 release binary it built
# moments before (docs/CONTRIBUTING.md, "Billed minutes": nothing built twice).
MANIFEST_BIN := $(MANIFEST_HOST_BIN)
else
MANIFEST_DIR := build/manifest-host
MANIFEST_BIN := $(MANIFEST_DIR)/openchimed
$(MANIFEST_BIN): FORCE
	@mkdir -p $(dir $@)
	@$(MAKE) --no-print-directory ARCH=$(HOST_ARCH) RELEASE=0 CC=cc CXX=c++ OC_ARCH= \
	    TTS=0 STT=0 SUM=0 WEB=0 REMEMBER=0 BIN=$@ \
	    GEN_DIR=$(MANIFEST_DIR)/gen OBJ_DIR=$(MANIFEST_DIR)/obj CC_STAMP=$(MANIFEST_DIR)/.cc \
	    BUILD_ID_STAMP=$(MANIFEST_DIR)/.build_id SQLITE_O=$(MANIFEST_DIR)/sqlite3.o ARGON2_O=$(MANIFEST_DIR)/argon2.o $@
endif
tts/manifest: $(MANIFEST_BIN) ttskit/data/voice.bin ttskit/data/VOICE-LICENSE ttskit/data/en-US/lexicon.bin ttskit/data/en-US/guesses.bin
	mkdir -p tts/en-US
	cp ttskit/data/voice.bin ttskit/data/VOICE-LICENSE tts/
	cp ttskit/data/en-US/lexicon.bin ttskit/data/en-US/guesses.bin tts/en-US/
	$(abspath $(MANIFEST_BIN)) --tts-manifest tts
stt/manifest: $(MANIFEST_BIN) sttkit/data/recognizer.bin sttkit/data/RECOGNIZER-LICENSE
	mkdir -p stt
	rm -f stt/*.ort stt/tokenizer.bin stt/streaming_config.json
	cp sttkit/data/recognizer.bin sttkit/data/RECOGNIZER-LICENSE stt/
	$(abspath $(MANIFEST_BIN)) --stt-manifest stt
