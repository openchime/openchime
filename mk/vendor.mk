# The vendored libraries the host build links, each fetched and built pinned by
# a script under scripts/build/ (docs/VENDORS.md §2), and the committed SQLite
# amalgamation. A build that takes minutes says so before it starts.

# --- SQLite (vendored, third_party/sqlite-3.53.4) -----------------------------
# The amalgamation, compiled into the daemon rather than linked from the host.
# The point is not the dependency count: the daemon otherwise inherits whatever
# SQLite the distro ships, INCLUDING whether FTS5 was compiled into it, and
# migrate.c creates messages_fts as a virtual table USING fts5. A host without
# that module fails at migration time, on a machine we do not control. Pinning
# the version makes it a build fact instead.
#
# Its own rule, with its own flags: 9 MB of generated C does not survive
# -Wall -Wextra -Werror, the same accommodation audio_dev.c makes for miniaudio.
# FTS5 is required, not tuning. THREADSAFE=1 is the system library's own default,
# so vendoring changes no concurrency semantics -- three threads hold their own
# connections (dbwriter's writer and reader, and push). The omissions are things
# this daemon provably does not use: no extension loading, no deprecated calls,
# no shared cache, and no double-quoted string literals in any of its SQL.
SQLITE_DEFS := -DSQLITE_ENABLE_FTS5 -DSQLITE_THREADSAFE=1 -DHAVE_USLEEP=1 \
               -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED \
               -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 \
               -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_OMIT_SHARED_CACHE
$(SQLITE_O): $(SQLITE_DIR)/sqlite3.c $(SQLITE_DIR)/sqlite3.h $(CC_STAMP) $(TOOLCHAIN_DEPS) | build
	@mkdir -p $(dir $@)
	$(CC) $(filter-out $(WARN_CFLAGS),$(CFLAGS)) -w $(SQLITE_DEFS) -c $< -o $@

# Argon2, as released, in one unit (argon2_all.c) with its own flags: the
# release compares an unsigned length with a zero minimum (-Wtype-limits), and
# the daemon hashes with one lane, so no threads.
$(ARGON2_O): $(wildcard $(ARGON2_DIR)/*.c $(ARGON2_DIR)/*.h $(ARGON2_DIR)/blake2/*) $(CC_STAMP) $(TOOLCHAIN_DEPS) | build
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-type-limits -DARGON2_NO_THREADS -I$(ARGON2_DIR) -c $(ARGON2_DIR)/argon2_all.c -o $@

# --- mbedTLS and the media codecs, natively ------------------------------------
$(MBEDTLS_A): $(TOOLCHAIN_DEPS)
	@echo "openchime: building mbedTLS (about a minute, once)"
	scripts/build/mbedtls.sh
$(LIBVPX_A): $(TOOLCHAIN_DEPS)
	@echo "openchime: building libvpx (a few minutes, once)"
	scripts/build/libvpx.sh native
$(OPUS_A): $(TOOLCHAIN_DEPS)
	scripts/build/opus.sh native
$(SPEEXDSP_A): $(TOOLCHAIN_DEPS)
	scripts/build/speexdsp.sh native

# --- Speech: ONNX Runtime and the models (ARCH-111, ARCH-112) -----------------
# A minimal ONNX Runtime built from source as one static archive, for read-aloud
# and voice input. The models are DATA, fetched at pinned SHA-256s into build/:
# Kitten (converted to .ort by scripts/build/tts_convert.c) and Moonshine's
# published files. `make clean` keeps the downloads.
ORT_DIR := third_party/onnxruntime-1.30.0$(ARCH_SUFFIX)
ORT_A   := $(ORT_DIR)/lib/libonnxruntime.a
KITTEN  := build/kitten/kitten.ort
$(ORT_A): daemon/ort.ops.config $(TOOLCHAIN_DEPS)
	@echo "openchime: building ONNX Runtime from source for read-aloud and voice input (about 20 minutes, once; make TTS=0 STT=0 skips it)"
	scripts/build/onnxruntime.sh
	@touch -c $@   # the script leaves a current build alone
# A cross build runs the host's protoc, which the script builds on its own
# when the host has none: protoc alone, from the same pinned sources, a few
# minutes rather than the host's whole archive (scripts/build/onnxruntime.sh).
# One run of the script makes both files (grouped, &:), so `make -j` never runs
# two downloads into the same files at once.
$(KITTEN) build/kitten/voices.npz &: scripts/build/kitten.sh scripts/build/tts_convert.c
	@echo "openchime: fetching the Kitten voice model (once)"
	scripts/build/kitten.sh
	@touch -c $(KITTEN) build/kitten/voices.npz   # the script leaves a current model alone
build/moonshine/.done: scripts/build/moonshine.sh
	@echo "openchime: fetching the Moonshine recognizer (once)"
	scripts/build/moonshine.sh
	@touch -c $@

# --- Summaries: llama.cpp (ARCH-116) ------------------------------------------
# Built from pinned source as one static archive. The model itself is DATA,
# fetched on first use when an operator turns summaries on, never shipped.
LLAMA_DIR := third_party/llamacpp-0.5.0$(ARCH_SUFFIX)
LLAMA_A   := $(LLAMA_DIR)/lib/libllamacpp.a
$(LLAMA_A): scripts/build/llamacpp.sh $(TOOLCHAIN_DEPS)
	@echo "openchime: building llama.cpp for summaries (a few minutes, once; make SUM=0 skips it)"
	scripts/build/llamacpp.sh
	@touch -c $@   # the script leaves a current build alone
# Never at the same time as ONNX Runtime, when both are built: each is a large
# C++ build using every CPU, and together, on a cold cache, they can run a
# small machine (2 CPUs, 7 GB) out of memory.
ifneq ($(filter 1,$(TTS) $(STT)),)
$(LLAMA_A): | $(ORT_A)
endif

# The common-password list (third_party/pwlist), compiled in (daemon/pwpolicy.c).
$(PWLIST_RES): third_party/pwlist/common.txt scripts/build/pw_common.py
	@mkdir -p $(dir $@)
	python3 scripts/build/pw_common.py $@ third_party/pwlist/common.txt
