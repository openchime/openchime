# The tree's source groups and include paths, named once for every rule file.
# The tree is split into three concerns: shared/ is the wire contract (linked by
# both the daemon and the client), daemon/ is the server, client/ is the app.

# The host's daemon at the root; a cross-built one under build/<arch>/.
BIN := $(if $(ARCH_SUFFIX),build$(BUILD_SUFFIX)/openchimed,openchimed)

SHARED_SRC := shared/protocol.c shared/framebuf.c shared/tls.c shared/mention.c \
              shared/searchq.c shared/notify.c shared/url.c shared/richtext.c shared/speakable.c shared/action.c \
              shared/oc_mp4.c shared/e2e_hpke.c shared/e2e_sframe.c \
              third_party/ca-roots/ca_roots.c
DAEMON_SRC := daemon/main.c daemon/config.c daemon/config_table.c daemon/migrate.c daemon/dbwriter.c daemon/netloop.c daemon/auth.c daemon/jwt.c daemon/joinrules.c daemon/proxyproto.c daemon/listen.c daemon/ratelimit.c daemon/roles.c daemon/blobstore.c daemon/blob_s3.c daemon/xferpool.c daemon/storage.c daemon/sigv4.c daemon/http.c third_party/picohttpparser/picohttpparser.c daemon/relay.c daemon/ioloop.c daemon/enroll.c daemon/push.c daemon/invite_mail.c daemon/unfurl.c daemon/voice_pick.c daemon/idmap.c daemon/srccount.c daemon/authpool.c daemon/https_client.c daemon/acme.c daemon/certs.c daemon/localissuer.c daemon/webpages.c daemon/webapp.c daemon/devicecodes.c daemon/json.c daemon/idtoken.c daemon/oidcrp.c daemon/relaykeys.c daemon/totp.c daemon/webstep.c daemon/cbor.c daemon/webauthn.c third_party/qrcodegen/qrcodegen.c daemon/sum_core.c daemon/sum_store.c daemon/sum_worker.c daemon/sum_load.c daemon/sum_fetch.c daemon/sum_cloud.c daemon/tts_data.c
# The web client's page and loader, compiled into the daemon as resources
# (daemon/webapp.h). The page always; the loader when WEB=1 (mk/web.mk), so a
# daemon built without Emscripten still links, and says it has no client.
WEBAPP_RES := build/gen/webapp_res.c
SRC        := $(SHARED_SRC) $(DAEMON_SRC) $(WEBAPP_RES)
HDRS       := $(wildcard shared/*.h daemon/*.h)

# Vendored, pinned mbedTLS (scripts/build/mbedtls.sh) -- one version across local
# builds, CI, the release, and the container image. Link order matters for static
# archives: tls -> x509 -> crypto.
MBEDTLS_DIR  := third_party/mbedtls-3.6.2$(ARCH_SUFFIX)
MBEDTLS_INC  := $(MBEDTLS_DIR)/include
MBEDTLS_A    := $(MBEDTLS_DIR)/library/libmbedtls.a
MBEDTLS_LIBS := $(MBEDTLS_DIR)/library/libmbedtls.a \
                $(MBEDTLS_DIR)/library/libmbedx509.a \
                $(MBEDTLS_DIR)/library/libmbedcrypto.a

SQLITE_DIR := third_party/sqlite-3.53.4
SQLITE_INC := -I$(SQLITE_DIR)
SQLITE_O   := build$(BUILD_SUFFIX)/sqlite3.o

INC := -Ishared -Idaemon -Ithird_party/jsmn -Ithird_party/picohttpparser -Ithird_party/qrcodegen -I$(MBEDTLS_INC)

# Every source except the daemon entry point; the test binary links these and
# calls their public APIs (no per-test binaries, no unity #include of .c).
APP_SRC  := $(SHARED_SRC) $(filter-out daemon/main.c,$(DAEMON_SRC)) $(WEBAPP_RES)
# e2e_client is a standalone black-box tool (its own main), not part of the
# single in-process test binary.
TEST_SRC := $(filter-out tests/e2e_client.c tests/demo_client.c tests/bench_load.c tests/bench_loop.c,$(wildcard tests/*.c))
TEST_BIN := build/tests

# --- Client app-core (ARCH-74) ------------------------------------------------
# The shared, frontend-agnostic core: the net thread + the two UI<->net queues +
# the view-model/reducers + the oc_client facade. It has no main of its own -- the
# headless test (make test) links and drives it against an in-process daemon, and
# frontends (a TUI first, then native GUIs) link it too. See docs/CLIENT.md.
# signin_web.c is the browser's signin.c (WEB.md): the web core takes it in
# place of signin.c, the native build leaves it out.
CORE_SRC := $(filter-out client/core/signin_web.c,$(wildcard client/core/*.c))
CORE_INC := -Iclient/core

# --- Video message media (ARCH-110) -------------------------------------------
# Capture, the audio device layer, the codec wrappers, the MP4 muxer/demuxer, the
# recorder and the player. Kept out of CORE_SRC so a client that does not record
# or play video (the TUI) links no codec. libvpx and libopus are fetched and built
# pinned (scripts/build/libvpx.sh, scripts/build/opus.sh), natively for `make
# test` and cross-built for the Win32 client.
MEDIA_SRC  := $(wildcard client/core/media/*.c)
MEDIA_HDRS := $(wildcard client/core/media/*.h)
LIBVPX_DIR := third_party/libvpx-1.17.0$(ARCH_SUFFIX)
OPUS_DIR   := third_party/opus-1.6.1$(ARCH_SUFFIX)
LIBVPX_A   := $(LIBVPX_DIR)/lib/libvpx.a
OPUS_A     := $(OPUS_DIR)/lib/libopus.a
# speexdsp's echo canceller sits behind the processor seam (AUDIO.md §3.3), for
# voice input first and calls after (ARCH-112); fetched and built pinned
# (scripts/build/speexdsp.sh) like the codecs.
SPEEXDSP_DIR := third_party/speexdsp-1.2.1$(ARCH_SUFFIX)
SPEEXDSP_A   := $(SPEEXDSP_DIR)/lib/libspeexdsp.a
MEDIA_INC  := -Iclient/core/media -I$(LIBVPX_DIR)/include -I$(OPUS_DIR)/include -I$(SPEEXDSP_DIR)/include
MEDIA_LIBS := $(LIBVPX_A) $(OPUS_A) $(SPEEXDSP_A) -ldl -lm

# --- Voice input (ARCH-112) ---------------------------------------------------
# The dictation session, the libfvad segmenter (vendored source,
# third_party/libfvad) and the capture thread, over the media library's device
# layer and processor. Linked by the Win32 client and the tests.
VOICE_SRC  := $(wildcard client/core/voice/*.c)
VOICE_HDRS := $(wildcard client/core/voice/*.h)
VOICE_INC  := -Iclient/core/voice -Ithird_party/libfvad/include

# --- Calls (ARCH-113, docs/CALLS.md) -----------------------------------------
# The media engine behind the core's oc_call_media seam: the microphone through
# the canceller and speexdsp's preprocessor, Opus, SFrame, the relay socket, the
# jitter buffers and the mixer. Linked by the Win32 client and the tests; the
# core does the signaling and the keys, and links no codec.
CALL_SRC  := $(wildcard client/core/call/*.c)
CALL_HDRS := $(wildcard client/core/call/*.h)
CALL_INC  := -Iclient/core/call

# --- ttskit (read-aloud pronunciation, ARCH-111) -----------------------------
# English text to the IPA a voice model reads: a CMUdict-derived dictionary and a
# trained guesser, both committed as memory-mapped data in ttskit/data. Pure C, no
# dependencies; linked into the daemon and covered by `make test`. tts_pack
# builds the data files (scripts/build/ttskit_data.sh) and says words with them.
# See docs/TTSKIT.md.
TTSKIT_SRC := $(filter-out ttskit/tts_pack.c,$(wildcard ttskit/*.c))
TTSKIT_INC := -Ittskit

# --- sdltext (portable text layer) --------------------------------------------
# One text API for every graphical client: layout, measurement, hit-testing,
# byte-offset range styling. The portable core (the byte<->UTF-16 offset map)
# compiles everywhere and is covered by `make test`; the DirectWrite backend
# compiles only into Windows builds. See sdltext/sdltext.h.
# The portable application layer of the GUI (ARCH-80): every graphical client
# compiles it over its platform directory (client/gui/platform/<os>/).
APP_GUI_SRC := $(wildcard client/gui/app/*.c)
UTF8PROC    := third_party/utf8proc/utf8proc.c
# The common core carries utf8proc (clusters, line breaks, bidi: st_priv.h)
# for a backend that lays text out itself; DirectWrite links it too, unused.
SDLTEXT_COMMON := sdltext/st_common.c $(UTF8PROC)
SDLTEXT_WIN    := sdltext/st_common.c sdltext/st_dwrite.c $(UTF8PROC)
SDLTEXT_INC    := -Isdltext
# theme.c is GUI source and is compiled into the tests anyway: it is colour
# arithmetic with one Windows call behind an #ifdef, and the contrast guarantee
# it carries has to be asserted somewhere that RUNS.
THEME_SRC := client/gui/app/theme.c
# The drawing layer (client/gui/gfx): fills, rounded rects, clips, lines,
# textures and the Lucide stroke tessellator over an SDL3 renderer.
GFX_SRC := client/gui/gfx/gfx_sdl.c client/gui/gfx/gfx_icons.c

# --- Client TUI frontend (ARCH-75) --------------------------------------------
# termbox2 (cell grid + input) + utf8proc (Unicode width/grapheme), both vendored
# as committed MIT single-file source. Builds on the host like the daemon.
TUI_SRC := $(wildcard client/tui/*.c) $(wildcard client/shared/secret_*.c)
# tuikit -- the in-tree TUI toolbox (terminal layer + widgets + formatting). Owns
# the termbox2 instantiation + the Windows console backend (ARCH-83). Linked into
# both the POSIX and Windows TUI. The QR code the device-code sign-in shows
# (tk_qr, AUTH.md §8.11): Nayuki's qrcodegen, MIT, vendored at v1.8.0.
QRCODEGEN  := third_party/qrcodegen/qrcodegen.c
TUIKIT_SRC := $(filter-out tuikit/demo.c,$(wildcard tuikit/*.c)) $(QRCODEGEN)
TUIKIT_INC := -Ituikit -Ithird_party/qrcodegen
# The core's local store (client/core/store.c) embeds NO database engine
# (ARCH-88/REQ-201): the credential store holds the token + pin, and the cache,
# outbox and workspace book are plain files. So a client links no sqlite.
# The platform credential backend (client/shared/secret_*.c, one entry point in
# secret_os.h): libsecret on Linux when the dev package is present, Windows
# Credential Manager on the Windows builds. Without libsecret it compiles a stub
# that reports "no keyring" -- `make linux` still builds, but that machine then
# persists no session token at all (a credential is never written to plaintext).
ifeq ($(shell pkg-config --exists libsecret-1 && echo yes),yes)
  SECRET_CFLAGS := $(shell pkg-config --cflags libsecret-1) -DOC_HAVE_LIBSECRET
  SECRET_LIBS   := $(shell pkg-config --libs libsecret-1)
else
  SECRET_CFLAGS :=
  SECRET_LIBS   :=
endif
TUI_INC := $(CORE_INC) -Iclient/tui -Iclient/shared -Ithird_party/termbox2 -Ithird_party/utf8proc
TUI_BIN := build/openchime-tui
