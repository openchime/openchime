# The Windows clients, cross-compiled to standalone .exe files: the GUI
# (ARCH-80) and the TUI (ARCH-81). The toolchain is llvm-mingw (clang, lld,
# LLVM's binutils, current mingw-w64 headers), fetched pinned on first use
# (mk/toolchains.mk); the x86_64-w64-mingw32-* names below are its wrappers.
# Both clients use the Windows mbedTLS (third_party/mbedtls-3.6.2-win, built
# by its rule below). No sqlite: a client embeds no database engine (ARCH-88).
WINCC      ?= $(MINGW_CC)
WINDRES    ?= $(dir $(MINGW_CC))x86_64-w64-mingw32-windres
WINOBJCOPY ?= $(dir $(MINGW_CC))x86_64-w64-mingw32-objcopy


# Both .exe files carry a VERSIONINFO resource from client/shared/res. Only the
# number is passed; the .rc builds the display string from it, for the reason
# recorded there. OC_VERSION is a bare integer or empty, and VERSIONINFO has no
# spelling for "dev", so an unset version stamps 0.
#
# make tracks file times, not variables, so changing OC_VERSION on a tree that
# already built leaves the old number in build/*_res.o. CI checks out clean and
# never sees this; a local rebuild at a new version wants `make clean` first.
OC_VERSION_NUM := $(if $(OC_VERSION),$(OC_VERSION),0)
WIN_RES_SRC    := client/shared/res/version_info.rc
WINDRES_ARGS   := -I client/shared/res --define OC_VERSION_NUM=$(OC_VERSION_NUM)
MBEDTLS_WIN    := third_party/mbedtls-3.6.2-win
WIN_MBEDLIBS   := $(MBEDTLS_WIN)/library/libmbedtls.a \
                  $(MBEDTLS_WIN)/library/libmbedx509.a \
                  $(MBEDTLS_WIN)/library/libmbedcrypto.a
$(WIN_MBEDLIBS) &: scripts/build/mbedtls_windows.sh $(MINGW_CC)
	@echo "openchime: building mbedTLS for Windows (about a minute, once)"
	scripts/build/mbedtls_windows.sh
	@touch -c $(WIN_MBEDLIBS)   # the script leaves a current build alone
# -g: debug info, so a crash RVA from the report (see crash_filter) resolves to a
# file and line via addr2line. Without it the report can only say "somewhere in
# app.c", which is not a lead. No runtime cost; the GUI's are split out below.
# The Windows flags are their own variable rather than an addition to CFLAGS, so
# they carry WARN_CFLAGS explicitly. They must: the cross build is where the two
# implicit float declarations hid, because nothing on the native side compiles
# app.c at all.
WIN_CFLAGS := -std=c99 $(WARN_CFLAGS) -O2 -g -D_WIN32_WINNT=0x0601 -DUTF8PROC_STATIC $(DEMO_DEF)

# --- The TUI ------------------------------------------------------------------
# The termbox2 backend is tuikit/tk_term.c (Console API); the core seams
# (threads/DNS/RNG) are in shared/oc_thread.h + resolve.c + net.c.
WIN_TUI_BIN := build/openchime-tui.exe
WIN_INC := -Ishared -Idaemon -Ithird_party/jsmn -I$(MBEDTLS_WIN)/include \
           $(CORE_INC) -Iclient/tui -Iclient/shared -Ituikit -Ithird_party/qrcodegen -Ithird_party/termbox2 -Ithird_party/utf8proc
# Version metadata only -- no icon, which for a console program comes from the
# host terminal window rather than the image.
WIN_TUI_RES := build/openchime_tui_res.o
$(WIN_TUI_RES): client/tui/res/openchime_tui.rc $(WIN_RES_SRC) $(MINGW_CC) | build
	$(WINDRES) $(WINDRES_ARGS) $< -O coff -o $@
$(WIN_TUI_BIN): $(TUI_SRC) $(TUIKIT_SRC) $(CORE_SRC) $(SHARED_SRC) $(UTF8PROC) $(WIN_TUI_RES) \
                $(wildcard client/tui/*.h tuikit/*.h client/core/*.h shared/*.h) $(WIN_MBEDLIBS) $(DEMO_STAMP) | build
	$(WINCC) $(WIN_CFLAGS) -Wno-unused-result $(WIN_INC) \
	    $(TUI_SRC) $(TUIKIT_SRC) $(CORE_SRC) $(SHARED_SRC) $(UTF8PROC) $(WIN_TUI_RES) \
	    $(WIN_MBEDLIBS) -lws2_32 -ldnsapi -lbcrypt -lcrypt32 -lole32 -ladvapi32 -lshell32 -static -o $@

# --- The GUI ------------------------------------------------------------------
# The portable application layer (client/gui/app: SDL3 + oc_gfx + sdltext) over
# the Windows platform (client/gui/platform/win32) and the same shared app-core.
# SDL supplies the entry point (SDL_main.h); -mwindows selects the GUI subsystem
# (no console). SDL3 is the "fetched at build" vendoring class (docs/VENDORS.md
# §2), pinned + cross-built by scripts/build/sdl3_windows.sh, statically linked.
SDL3_VERSION := 3.4.14
SDL3_WIN     := third_party/sdl3-$(SDL3_VERSION)-win
SDL3_WIN_LIB := $(SDL3_WIN)/lib/libSDL3.a
# The OS libraries a static SDL3 needs beyond what the GUI already links.
WIN_SDL_SYSLIBS := -lm -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 \
                   -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8
$(SDL3_WIN_LIB): $(MINGW_CC)
	@echo "openchime: building SDL3 for Windows (a few minutes, once)"
	scripts/build/sdl3_windows.sh

WIN_GUI_BIN := build/openchime.exe
# Video messages (ARCH-110): the cross-built codecs, and Media Foundation for the
# camera. The native rules build Linux archives only, so like mbedTLS these have
# a rule of their own here.
WIN_LIBVPX_A   := third_party/libvpx-1.17.0-win/lib/libvpx.a
WIN_OPUS_A     := third_party/opus-1.6.1-win/lib/libopus.a
WIN_SPEEXDSP_A := third_party/speexdsp-1.2.1-win/lib/libspeexdsp.a
WIN_MEDIA_A    := $(WIN_LIBVPX_A) $(WIN_OPUS_A) $(WIN_SPEEXDSP_A)
WIN_MEDIA_INC  := -Iclient/core/media -Ithird_party/libvpx-1.17.0-win/include -Ithird_party/opus-1.6.1-win/include \
                  -Ithird_party/speexdsp-1.2.1-win/include $(VOICE_INC)
# -lpthread: libvpx's mingw build threads through winpthreads (statically linked
# here, like everything else, so no DLL ships beside the .exe).
WIN_MEDIA_SYSLIBS := -lmfplat -lmfreadwrite -lmf -lmfuuid -lole32 -luuid -lpthread
$(WIN_LIBVPX_A): $(MINGW_CC)
	scripts/build/libvpx.sh windows
$(WIN_OPUS_A): $(MINGW_CC)
	scripts/build/opus.sh windows
$(WIN_SPEEXDSP_A): $(MINGW_CC)
	scripts/build/speexdsp.sh windows
# Debug symbols, split out of the shipped binary (see the strip step below).
WIN_GUI_SYMS := build/openchime.debug
GUI_SRC := $(APP_GUI_SRC) $(wildcard client/gui/platform/win32/*.c) client/shared/icons.c client/shared/secret_win.c \
           client/shared/osnotify_win.c client/shared/feedback.c \
           $(SDLTEXT_WIN) $(GFX_SRC)
WIN_GUI_INC := -Ishared -Idaemon -Ithird_party/jsmn -I$(MBEDTLS_WIN)/include \
               $(CORE_INC) $(CALL_INC) -Iclient/gui/app -Iclient/gui/platform -Iclient/gui/platform/win32 -Iclient/shared \
               $(SDLTEXT_INC) -Iclient/gui/gfx -I$(SDL3_WIN)/include
# Resources (app icon + VERSIONINFO). Regenerate the .ico with scripts/gen/appicon.py.
WIN_GUI_RES := build/openchime_res.o
$(WIN_GUI_RES): client/gui/platform/win32/res/openchime.rc client/gui/platform/win32/res/openchime.ico \
                client/gui/platform/win32/res/openchime_res.h $(WIN_RES_SRC) $(MINGW_CC) | build
	$(WINDRES) -I client/gui/platform/win32/res $(WINDRES_ARGS) $< -O coff -o $@
$(WIN_GUI_BIN): $(GUI_SRC) $(CORE_SRC) $(MEDIA_SRC) $(VOICE_SRC) $(CALL_SRC) $(SHARED_SRC) $(WIN_GUI_RES) \
                $(wildcard client/gui/app/*.h client/gui/platform/*.h client/gui/platform/win32/*.h client/core/*.h shared/*.h sdltext/*.h client/gui/gfx/*.h) $(MEDIA_HDRS) $(VOICE_HDRS) $(CALL_HDRS) \
                $(WIN_MBEDLIBS) $(SDL3_WIN_LIB) $(WIN_MEDIA_A) $(DEMO_STAMP) | build
	$(WINCC) $(WIN_CFLAGS) -std=c11 -Wno-unused-result -mwindows $(WIN_GUI_INC) $(WIN_MEDIA_INC) -Iclient/gui/platform/win32/res \
	    $(GUI_SRC) $(CORE_SRC) $(MEDIA_SRC) $(VOICE_SRC) $(CALL_SRC) $(SHARED_SRC) $(WIN_GUI_RES) \
	    $(WIN_MBEDLIBS) $(WIN_MEDIA_A) -L$(SDL3_WIN)/lib -lSDL3 -lws2_32 -ldnsapi -lbcrypt -lcrypt32 -lcomdlg32 \
	    -ld2d1 -ldwrite -lwindowscodecs -ldwmapi -limm32 $(WIN_MEDIA_SYSLIBS) $(WIN_SDL_SYSLIBS) -static -o $@
# Split the debug info out rather than discarding it. The client writes real
# minidumps on a crash (crash_filter, client/gui/platform/win32/plat_win32.c),
# and symbolicating a mingw build needs its DWARF -- a plain strip would shrink
# the download by trading away every future crash report. So: keep the symbols
# beside the binary, strip the shipped one, and record a debuglink so a
# debugger loads them back.
	$(WINOBJCOPY) --only-keep-debug $@ $(WIN_GUI_SYMS)
	$(WINOBJCOPY) --strip-debug --strip-unneeded $@
	$(WINOBJCOPY) --add-gnu-debuglink=$(WIN_GUI_SYMS) $@

# Both clients, once their binaries are named above.
windows: $(WIN_GUI_BIN) $(WIN_TUI_BIN)

# --- The two Windows test programs, built by path -----------------------------
# The DirectWrite backend's (sdltext/st_test_win.c): a console .exe whose exit
# code is its failure count; CI cross-compiles it (the compile is itself the
# header/vtable check mingw keeps honest), a Windows host -- or WSL interop --
# runs it. docs/TESTING.md records the split.
WIN_SDLTEXT_TEST := build/sdltext_test.exe
$(WIN_SDLTEXT_TEST): $(SDLTEXT_WIN) sdltext/st_test_win.c $(wildcard sdltext/*.h) $(MINGW_CC) | build
	$(WINCC) $(WIN_CFLAGS) $(SDLTEXT_INC) \
	    $(SDLTEXT_WIN) sdltext/st_test_win.c \
	    -ld2d1 -ldwrite -lwindowscodecs -lole32 -luuid -static -o $@
# The gfx layer's: SDL's software renderer on a plain surface -- no window, no
# GPU -- asserting pixels. Exit code is the failure count (docs/TESTING.md, the
# platform-backend exception).
WIN_GFX_TEST := build/gfx_test.exe
$(WIN_GFX_TEST): $(GFX_SRC) client/gui/gfx/gfx_test_win.c client/shared/icons.c \
                 $(wildcard client/gui/gfx/*.h) $(SDL3_WIN_LIB) $(MINGW_CC) | build
	$(WINCC) $(WIN_CFLAGS) -std=c11 -Iclient/gui/gfx -Iclient/shared -I$(SDL3_WIN)/include \
	    $(GFX_SRC) client/gui/gfx/gfx_test_win.c client/shared/icons.c \
	    -L$(SDL3_WIN)/lib -lSDL3 $(WIN_SDL_SYSLIBS) -static -o $@
