# The Linux clients: the TUI (app-core + shared wire + termbox2/utf8proc), which
# `linux` builds, and the GUI below. -Wno-unused-result relaxes one
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

# --- The GUI ------------------------------------------------------------------
# The portable application layer (client/gui/app: SDL3 + oc_gfx + sdltext) over
# the Linux platform (client/gui/platform/linux) and the same shared app-core,
# media, voice and call code as Windows (mk/windows.mk, "The GUI"), whose rules
# these follow. `make linux-gui` builds it; neither `make build` nor `make
# test` does, because it needs system development packages a host running
# only those may lack.
#
# SDL3 is the committed third_party/SDL3 (docs/details/VENDORS.md §1), built
# natively and statically by scripts/build/sdl3_linux.sh with the X11 and
# Wayland video backends; it opens their libraries itself when it starts, so
# the link names neither.
SDL3_LINUX     := third_party/sdl3-$(SDL3_VERSION)
SDL3_LINUX_LIB := $(SDL3_LINUX)/lib/libSDL3.a
$(SDL3_LINUX_LIB):
	@echo "openchime: building SDL3 for Linux (a few minutes, once)"
	scripts/build/sdl3_linux.sh
# The system packages it compiles and links against (docs/details/VENDORS.md
# §3): FreeType, HarfBuzz and fontconfig for text (FT_PKGS), GIO for D-Bus,
# libsecret for the credential store, libpipewire for capture. Asked of
# pkg-config once, and only when a rule below expands them (the first
# expansion replaces each variable with its value), so `make` and `make test`
# never ask.
LINUX_GUI_PKGS    := $(FT_PKGS) gio-2.0 libsecret-1 libpipewire-0.3
LINUX_GUI_PKG_CFLAGS = $(eval LINUX_GUI_PKG_CFLAGS := $(shell pkg-config --cflags $(LINUX_GUI_PKGS) 2>/dev/null))$(LINUX_GUI_PKG_CFLAGS)
LINUX_GUI_PKG_LIBS   = $(eval LINUX_GUI_PKG_LIBS := $(shell pkg-config --libs $(LINUX_GUI_PKGS) 2>/dev/null))$(LINUX_GUI_PKG_LIBS)
# What has to hold before the first object compiles: the packages are there.
# A file never written, so its recipe runs on every `make linux-gui`; it is
# order-only below, so it rebuilds nothing, and it fails naming what is missing
# before a compiler is asked for a header it cannot find.
LINUX_GUI_PKGS_OK := build/.linux_gui_pkgs
$(LINUX_GUI_PKGS_OK): FORCE
	$(call oc_need_pkgs,linux-gui,$(LINUX_GUI_PKGS))

LINUX_GUI_BIN := build/openchime-gui
# Notifications go to the desktop's notification server over GIO
# (osnotify_linux.c).
LINUX_GUI_SRC := $(APP_GUI_SRC) $(wildcard client/gui/platform/linux/*.c) client/shared/icons.c \
                 client/shared/secret_libsecret.c client/shared/osnotify_linux.c client/shared/feedback.c \
                 $(SDLTEXT_FT) $(GFX_SRC)
LINUX_GUI_INC := -Ishared -Idaemon -Ithird_party/jsmn -I$(MBEDTLS_INC) \
                 $(CORE_INC) $(MEDIA_INC) $(VOICE_INC) $(CALL_INC) -Iclient/gui/app -Iclient/gui/platform \
                 -Iclient/gui/platform/linux -Iclient/shared $(SDLTEXT_INC) -Iclient/gui/gfx -I$(SDL3_LINUX)/include \
                 $(PREVIEW_INC) -Ithird_party/stb
# -g: a crash report's addresses resolve to a file and line, as on Windows.
# The credential store is libsecret's here, always (secret_libsecret.c): the
# package is required, not detected as the TUI's is.
# Two of GCC's warnings are relaxed for this build alone, which is the first
# to put the application layer through GCC (Windows and the browser compile it
# with clang). -Wno-format-truncation: the application formats into fixed
# buffers with snprintf throughout, where cutting a long name short is the
# intended result, and GCC warns at each of them that it may happen.
# -Wno-misleading-indentation: GCC reads a statement macro written on one
# line (WHO_ADD in app.c) as a misleading `for`.
# OC_HAVE_PORTAL_CAPTURE builds the capture backends of client/core/media/
# cap_portal.c, which need GIO and libpipewire: in every other build that
# compiles the media sources (the tests', Windows', the browser's) the file is
# empty, as cap_mf.c and cap_wgc.c are off Windows.
LINUX_GUI_CC   = $(CC) $(CFLAGS) $(VERSION_DEF) -std=c11 -g -Wno-unused-result -Wno-format-truncation -Wno-misleading-indentation \
                 -DUTF8PROC_STATIC -DOC_HAVE_LIBSECRET -DOC_HAVE_PORTAL_CAPTURE $(LINUX_GUI_INC) $(LINUX_GUI_PKG_CFLAGS)
# The vendored libraries: the TLS, SDL3, the image decoders, and the media
# codecs the native test binary links (mk/tests.mk), each built by its own rule.
LINUX_GUI_A    := $(MBEDTLS_A) $(SDL3_LINUX_LIB) $(PREVIEW_A) $(LIBVPX_A) $(OPUS_A) $(SPEEXDSP_A)
LINUX_GUI_OBJS := $(call oc_objs,$(OBJ_DIR)/linux-gui,$(LINUX_GUI_SRC) $(CORE_SRC) $(MEDIA_SRC) $(VOICE_SRC) $(CALL_SRC) $(SHARED_SRC))
$(eval $(call oc_obj_rules,$(OBJ_DIR)/linux-gui,LINUX_GUI_CC,$(CC_STAMP) $(BUILD_ID_STAMP),$(LINUX_GUI_PKGS_OK) $(LINUX_GUI_A)))
-include $(LINUX_GUI_OBJS:.o=.d)
$(LINUX_GUI_BIN): $(LINUX_GUI_OBJS) $(LINUX_GUI_A) | $(LINUX_GUI_PKGS_OK) build-dir
	$(CC) $(CFLAGS) -std=c11 -g $(LINUX_GUI_OBJS) $(MBEDTLS_LIBS) $(PREVIEW_LIBS) $(MEDIA_LIBS) \
	    $(SDL3_LINUX_LIB) $(LINUX_GUI_PKG_LIBS) -lresolv -lpthread -ldl -lm -o $@
linux-gui: $(LINUX_GUI_BIN)
