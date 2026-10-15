# The browser client (docs/WEB.md): the same application layer as Windows
# (client/gui/app) over the browser platform (client/gui/platform/web), the
# canvas text backend, the core, and stand-ins for what a browser lacks.
# Emscripten is fetched pinned on first use (mk/toolchains.mk); mbedTLS, SDL3
# and the codecs for wasm are built once beside the native ones
# (scripts/build/*_wasm.sh), with the toolchain's directory on their PATH.
#
# `make web` builds it. WEB=1 (Makefile, "Features") also compiles its loader
# into the daemon, which then serves the client at /app/.
WEB_DIR     := build/web
WEB_MBEDTLS := build/wasm/mbedtls/library
WEB_SDL3    := build/wasm/sdl3
WEB_CFLAGS  := -std=gnu11 -D_GNU_SOURCE -O2 $(WARN_CFLAGS) -pthread

$(WEB_MBEDTLS)/libmbedtls.a: $(EMCC)
	@echo "openchime: building mbedTLS for wasm (about a minute, once)"
	scripts/build/mbedtls_wasm.sh
	@touch -c $@
$(WEB_SDL3)/lib/libSDL3.a: $(EMCC)
	@echo "openchime: building SDL3 for wasm (a few minutes, once)"
	scripts/build/sdl3_wasm.sh
	@touch -c $@
# Media in the browser: the portable media, voice and call code as the desktop
# links it, over the browser's capture and audio backends in place of Media
# Foundation, Graphics Capture and miniaudio; the codecs built for wasm.
WEB_MEDIA_LIBS := build/wasm/libvpx/lib/libvpx.a build/wasm/opus/lib/libopus.a build/wasm/speexdsp/lib/libspeexdsp.a
$(WEB_MEDIA_LIBS) &: $(EMCC)
	@echo "openchime: building libvpx, opus and speexdsp for wasm (a few minutes, once)"
	scripts/build/media_wasm.sh
	@touch -c $(WEB_MEDIA_LIBS)

# The drawing layer's own pixel test (client/gui/gfx), built for wasm and run
# under Node by `make web-test`.
WEB_GFX_SRC   := $(GFX_SRC) client/shared/icons.c
WEB_GFX_FLAGS := -std=c99 -O2 $(WARN_CFLAGS) -Wno-typedef-redefinition -pthread \
                 -Iclient/gui/gfx -Iclient/shared -I$(WEB_SDL3)/include
WEB_GFX_LD    := $(WEB_SDL3)/lib/libSDL3.a -pthread -sALLOW_MEMORY_GROWTH=1 -Wno-pthreads-mem-growth
$(WEB_DIR)/gfx-test.js: $(WEB_GFX_SRC) client/gui/gfx/gfx_test_win.c client/gui/gfx/gfx.h $(WEB_SDL3)/lib/libSDL3.a $(EMCC)
	mkdir -p $(WEB_DIR)
	$(EMCC) $(WEB_GFX_FLAGS) $(WEB_GFX_SRC) client/gui/gfx/gfx_test_win.c $(WEB_GFX_LD) -sENVIRONMENT=node,worker -o $@

# The client. Asyncify lets its own loop yield to the page.
WEB_MEDIA_SRC := $(filter-out client/core/media/audio_ma.c client/core/media/cap_mf.c client/core/media/cap_wgc.c,$(MEDIA_SRC)) \
                 $(VOICE_SRC) $(CALL_SRC) client/gui/platform/web/cap_web.c client/gui/platform/web/audio_web.c \
                 client/gui/platform/web/devices_web.c
# Extra link flags for a one-off build (`make web WEB_LDEXTRA=--profiling-funcs`
# keeps the function names a browser's debugger shows in a wasm stack).
WEB_LDEXTRA ?=
WEB_GUI_SRC := $(APP_GUI_SRC) client/gui/platform/web/plat_web.c client/gui/platform/web/a11y_web.c \
               client/gui/platform/web/secret_mem.c client/shared/icons.c client/shared/feedback.c \
               client/shared/osnotify_web.c sdltext/st_canvas.c $(SDLTEXT_COMMON) $(GFX_SRC) \
               $(WEB_MEDIA_SRC)
WEB_GUI_INC := -Ishared -Iclient/core -Iclient/core/media -Iclient/core/voice -Iclient/core/call \
               -Iclient/gui/app -Iclient/gui/platform -Iclient/shared -Isdltext -Iclient/gui/gfx -Ithird_party/jsmn \
               -I$(WEB_SDL3)/include -Ibuild/wasm/mbedtls/include \
               -Ibuild/wasm/libvpx/include -Ibuild/wasm/opus/include -Ibuild/wasm/speexdsp/include -Ithird_party/libfvad/include
WEB_GUI_CFLAGS := -std=gnu11 -D_GNU_SOURCE -O2 $(WARN_CFLAGS) -pthread -Wno-typedef-redefinition
WEB_GUI_OBJS := $(patsubst %.c,build/wasm/gui/%.o,$(WEB_GUI_SRC))
# The application says its build (About OpenChime): compiled with the id, and
# again when the id changes.
build/wasm/gui/client/gui/app/app.o: WEB_GUI_CFLAGS += $(VERSION_DEF)
build/wasm/gui/client/gui/app/app.o: $(BUILD_ID_STAMP)
# The browser has no loopback listener for a sign-in's return: the core's
# signin.c is replaced by signin_web.c, the same API over the page (WEB.md).
WEB_CORE_SRC  := $(filter-out client/core/signin.c,$(CORE_SRC)) client/core/signin_web.c
WEB_CORE_OBJS := $(patsubst %.c,build/wasm/core/%.o,$(WEB_CORE_SRC) $(SHARED_SRC))
build/wasm/gui/%.o: %.c client/gui/platform/platform.h client/gui/app/keys.h $(WEB_SDL3)/lib/libSDL3.a $(WEB_MBEDTLS)/libmbedtls.a $(WEB_MEDIA_LIBS) $(EMCC)
	@mkdir -p $(dir $@)
	$(EMCC) $(WEB_GUI_CFLAGS) $(WEB_GUI_INC) -c $< -o $@
build/wasm/core/%.o: %.c $(WEB_MBEDTLS)/libmbedtls.a $(EMCC)
	@mkdir -p $(dir $@)
	$(EMCC) $(WEB_CFLAGS) -Ishared $(CORE_INC) -Iclient/shared -Ithird_party/jsmn -Ibuild/wasm/mbedtls/include -c $< -o $@
$(WEB_DIR)/openchime.js: $(WEB_GUI_OBJS) $(WEB_CORE_OBJS) client/gui/platform/web/openchime.html $(EMCC)
	mkdir -p $(WEB_DIR)
	$(EMCC) $(WEB_GUI_OBJS) $(WEB_CORE_OBJS) $(WEB_MBEDTLS)/libmbedtls.a $(WEB_MBEDTLS)/libmbedx509.a $(WEB_MBEDTLS)/libmbedcrypto.a $(WEB_SDL3)/lib/libSDL3.a \
	    $(WEB_MEDIA_LIBS) \
	    -pthread -sPTHREAD_POOL_SIZE=16 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=128MB -sSTACK_SIZE=1MB -sASYNCIFY -sASYNCIFY_STACK_SIZE=65536 \
	    -sAUDIO_WORKLET -sWASM_WORKERS -sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE='$$emscriptenGetAudioObject' \
	    -sENVIRONMENT=web,worker -sWEBSOCKET_SUBPROTOCOL=binary -sEXIT_RUNTIME=0 -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,UTF16ToString,stringToNewUTF8,stringToUTF8,FS,ENV \
	    -sFORCE_FILESYSTEM -Wno-pthreads-mem-growth -Wno-limited-postlink-optimizations $(WEB_LDEXTRA) -o $@
	cp client/gui/platform/web/openchime.html $(WEB_DIR)/openchime.html
	# The daemon serves the wasm from web/ beside its binary (daemon/webapp.h),
	# as it finds its voice data: placed there for a developer's daemon, and
	# copied into the packages the same way by the release.
	mkdir -p web
	cp $(WEB_DIR)/openchime.wasm web/
web: $(WEB_DIR)/openchime.js

# The page and the loader as C arrays for the daemon (daemon/webapp.h). The
# page comes straight from the source and needs no Emscripten; the loader is
# the built one when WEB=1 and empty otherwise, so the daemon still links and
# reports that it has no client. Rebuilt when the switch changes ($(FEATURES)).
ifeq ($(WEB),1)
WEBAPP_JS := $(WEB_DIR)/openchime.js
else
WEBAPP_JS :=
endif
$(WEBAPP_RES): client/gui/platform/web/openchime.html $(WEBAPP_JS) scripts/build/embed_res.py $(FEATURES)
	mkdir -p build/gen
	python3 scripts/build/embed_res.py $@ oc_webapp_html=client/gui/platform/web/openchime.html oc_webapp_js=$(WEBAPP_JS)

# The client's checks: the gfx pixel test under Node, then the client signing
# in, drawing, recording and composing in a headless Chromium
# (scripts/web/test.sh), against a daemon built with the client in it.
web-test: $(WEB_DIR)/gfx-test.js $(WEB_DIR)/openchime.js
	@$(MAKE) --no-print-directory WEB=1 $(BIN)
	$(NODE) $(WEB_DIR)/gfx-test.js
	scripts/web/test.sh
	scripts/web/pages.sh
