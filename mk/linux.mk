# The Linux client: the TUI today (app-core + shared wire + termbox2/utf8proc);
# the GUI joins this target when it exists. -Wno-unused-result relaxes one
# warning from the vendored termbox2 header (its read/write/strerror_r calls).
# One object per source, in a tree of its own (mk/sources.mk, "Objects"), so
# `make -j` compiles them all at once; the build id is the daemon's scheme:
# a change of it recompiles the objects, as a change of the compiler does.
linux: $(TUI_BIN)
TUI_CC   = $(CC) $(CFLAGS) $(VERSION_DEF) -Wno-unused-result $(INC) $(TUI_INC) $(TUIKIT_INC) $(SECRET_CFLAGS)
TUI_OBJS := $(call oc_objs,$(OBJ_DIR)/tui,$(TUI_SRC) $(TUIKIT_SRC) $(CORE_SRC) $(SHARED_SRC) $(UTF8PROC))
$(eval $(call oc_obj_rules,$(OBJ_DIR)/tui,TUI_CC,$(CC_STAMP) $(BUILD_ID_STAMP),$(MBEDTLS_A)))
-include $(TUI_OBJS:.o=.d)
$(TUI_BIN): $(TUI_OBJS) $(MBEDTLS_A) | build-dir
	$(CC) $(CFLAGS) $(TUI_OBJS) $(MBEDTLS_LIBS) -lresolv -lpthread $(SECRET_LIBS) -o $@
