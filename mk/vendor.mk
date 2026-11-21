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
# so vendoring changes no concurrency semantics -- several threads hold their
# own connections (the dbwriter's writer and its three readers, push, the
# summary worker and the blob sweep's reference check). The omissions are things
# this daemon provably does not use: no extension loading, no deprecated calls,
# no shared cache, and no double-quoted string literals in any of its SQL.
SQLITE_DEFS := -DSQLITE_ENABLE_FTS5 -DSQLITE_THREADSAFE=1 -DHAVE_USLEEP=1 \
               -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED \
               -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 \
               -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_OMIT_SHARED_CACHE
$(SQLITE_O): $(SQLITE_DIR)/sqlite3.c $(SQLITE_DIR)/sqlite3.h $(CC_STAMP) | build-dir
	@mkdir -p $(dir $@)
	$(CC) $(filter-out $(WARN_CFLAGS),$(CFLAGS)) -w $(SQLITE_DEFS) -c $< -o $@

# Argon2, as released, in one unit (argon2_all.c) with its own flags: the
# release compares an unsigned length with a zero minimum (-Wtype-limits), and
# the daemon hashes with one lane, so no threads.
$(ARGON2_O): $(wildcard $(ARGON2_DIR)/*.c $(ARGON2_DIR)/*.h $(ARGON2_DIR)/blake2/*) $(CC_STAMP) | build-dir
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-type-limits -DARGON2_NO_THREADS -I$(ARGON2_DIR) -c $(ARGON2_DIR)/argon2_all.c -o $@

# --- mbedTLS and the media codecs, natively ------------------------------------
$(MBEDTLS_A):
	@echo "openchime: building mbedTLS (about a minute, once)"
	scripts/build/mbedtls.sh
$(LIBVPX_A):
	@echo "openchime: building libvpx (a few minutes, once)"
	scripts/build/libvpx.sh native
$(OPUS_A):
	scripts/build/opus.sh native
$(SPEEXDSP_A):
	scripts/build/speexdsp.sh native
$(LIBAVC_A):
	scripts/build/libavc.sh
$(LAME_A):
	scripts/build/lame.sh

# The common-password list (third_party/pwlist), compiled in (daemon/pwpolicy.c).
$(PWLIST_RES): third_party/pwlist/common.txt scripts/build/pw_common.py
	@mkdir -p $(dir $@)
	python3 scripts/build/pw_common.py $@ third_party/pwlist/common.txt
