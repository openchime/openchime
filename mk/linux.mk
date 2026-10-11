# The Linux client: the TUI today (app-core + shared wire + termbox2/utf8proc);
# the GUI joins this target when it exists. -Wno-unused-result relaxes one
# warning from the vendored termbox2 header (its read/write/strerror_r calls).
linux: $(TUI_BIN)
$(TUI_BIN): $(TUI_SRC) $(TUIKIT_SRC) $(CORE_SRC) $(SHARED_SRC) $(UTF8PROC) \
            $(wildcard client/tui/*.h tuikit/*.h client/core/*.h shared/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -Wno-unused-result $(INC) $(TUI_INC) $(TUIKIT_INC) $(SECRET_CFLAGS) \
	    $(TUI_SRC) $(TUIKIT_SRC) $(CORE_SRC) $(SHARED_SRC) $(UTF8PROC) $(MBEDTLS_LIBS) -lresolv -lpthread $(SECRET_LIBS) -o $@
