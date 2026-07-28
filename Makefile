CC ?= cc
CFLAGS ?= -std=c99 -D_GNU_SOURCE -Wall -Wextra -O2
LDFLAGS ?= -lsqlite3 -lpthread

BIN := openchimed

# The tree is split into three concerns: shared/ is the wire contract (linked by
# both the daemon and the client), daemon/ is the server, client/ is the app.
SHARED_SRC := shared/protocol.c shared/framebuf.c shared/tls.c
DAEMON_SRC := daemon/main.c daemon/config.c daemon/migrate.c daemon/dbwriter.c daemon/netloop.c daemon/auth.c daemon/jwt.c daemon/ratelimit.c daemon/roles.c daemon/blobstore.c daemon/blob_s3.c daemon/xferpool.c daemon/storage.c daemon/sigv4.c daemon/http.c daemon/audio_sidecar.c daemon/enroll.c daemon/push.c
SRC        := $(SHARED_SRC) $(DAEMON_SRC)
HDRS       := $(wildcard shared/*.h daemon/*.h)

# Vendored, pinned mbedTLS (scripts/build_mbedtls.sh) — one version across
# local/CI/Docker. Link order matters for static archives: tls -> x509 -> crypto.
MBEDTLS_DIR  := third_party/mbedtls-3.6.2
MBEDTLS_INC  := $(MBEDTLS_DIR)/include
MBEDTLS_A    := $(MBEDTLS_DIR)/library/libmbedtls.a
MBEDTLS_LIBS := $(MBEDTLS_DIR)/library/libmbedtls.a \
                $(MBEDTLS_DIR)/library/libmbedx509.a \
                $(MBEDTLS_DIR)/library/libmbedcrypto.a

INC := -Ishared -Idaemon -Ithird_party/jsmn -I$(MBEDTLS_INC)

# Every source except the daemon entry point; the test binary links these and
# calls their public APIs (no per-test binaries, no unity #include of .c).
APP_SRC   := $(SHARED_SRC) $(filter-out daemon/main.c,$(DAEMON_SRC))
# e2e_client is a standalone black-box tool (its own main), not part of the
# single in-process test binary.
TEST_SRC  := $(filter-out tests/e2e_client.c tests/demo_client.c tests/bench_load.c,$(wildcard tests/*.c))
TEST_BIN  := build/tests

# --- Client app-core (ARCH-74) ------------------------------------------------
# The shared, frontend-agnostic core: the net thread + the two UI<->net queues +
# the view-model/reducers + the oc_client facade. It has no main of its own — the
# headless test (make test) links and drives it against an in-process daemon, and
# frontends (a TUI first, then native GUIs) link it too. See docs/CLIENT.md.
CORE_SRC := $(wildcard client/core/*.c)
CORE_INC := -Iclient/core

# --- Client TUI frontend (ARCH-75) --------------------------------------------
# termbox2 (cell grid + input) + utf8proc (Unicode width/grapheme), both vendored
# as committed MIT single-file source. Builds on the host like the daemon.
TUI_SRC   := $(wildcard client/tui/*.c) $(wildcard client/shared/secret_*.c)
# tuikit — the in-tree TUI toolbox (terminal layer + widgets + formatting). Owns
# the termbox2 instantiation + the Windows console backend (ARCH-83). Linked into
# both the POSIX and Windows TUI.
TUIKIT_SRC := $(filter-out tuikit/demo.c,$(wildcard tuikit/*.c))
TUIKIT_INC := -Ituikit
UTF8PROC  := third_party/utf8proc/utf8proc.c
# The core's local store (client/core/store.c) embeds NO database engine
# (ARCH-88/REQ-201): the credential store holds the token + pin, and the cache,
# outbox and workspace book are plain files. So a client links no sqlite and no
# migration runner — STORE_DEPS is gone with them.
# The platform credential backend (client/shared/secret_*.c, one entry point in
# secret_os.h): libsecret on Linux when the dev package is present, Windows
# Credential Manager on the Windows builds. Without libsecret it compiles a stub
# that reports "no keyring" — `make tui` still builds, but that machine then
# persists no session token at all (a credential is never written to plaintext).
ifeq ($(shell pkg-config --exists libsecret-1 && echo yes),yes)
  SECRET_CFLAGS := $(shell pkg-config --cflags libsecret-1) -DOC_HAVE_LIBSECRET
  SECRET_LIBS   := $(shell pkg-config --libs libsecret-1)
else
  SECRET_CFLAGS :=
  SECRET_LIBS   :=
endif
TUI_INC   := $(CORE_INC) -Iclient/tui -Iclient/shared -Ithird_party/termbox2 -Ithird_party/utf8proc
TUI_BIN   := build/openchime-tui

.PHONY: all run test integration core tui bench clean s3-smoke windows-tui windows-gui tuikit-demo demo-client

all: $(BIN)

# One-shot local dev server: builds the daemon + TUI and runs the daemon on
# 127.0.0.1:8443 with sane paths + bootstrap users (alice/pw, bob/pw). Connect
# from another terminal: build/openchime-tui 127.0.0.1 8443 alice:pw
run: $(BIN) $(TUI_BIN)
	@mkdir -p /tmp/openchime-dev/blobs
	@echo "openchime dev daemon -> 127.0.0.1:8443  (users: alice/pw, bob/pw)"
	@echo "connect: build/openchime-tui 127.0.0.1 8443 alice:pw"
	@OPENCHIME_DB_PATH=/tmp/openchime-dev/db OPENCHIME_BLOB_DIR=/tmp/openchime-dev/blobs \
	 OPENCHIME_TLS_CERT=/tmp/openchime-dev/cert.pem OPENCHIME_TLS_KEY=/tmp/openchime-dev/key.pem \
	 OPENCHIME_PROTO_PORT=8443 OPENCHIME_HEALTH_PORT=8080 \
	 OPENCHIME_BOOTSTRAP_USERS="alice:pw:owner,bob:pw:member" ./$(BIN)

$(BIN): $(SRC) $(MBEDTLS_A) $(HDRS)
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC) $(MBEDTLS_LIBS) $(LDFLAGS)

$(MBEDTLS_A):
	scripts/build_mbedtls.sh

# Unit + in-process integration tests, one binary (docs/TESTING.md §2). Built
# -O0 -g; a non-zero exit fails the build and CI.
test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(APP_SRC) $(CORE_SRC) $(HDRS) $(wildcard tests/*.h client/core/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O0 -g $(INC) $(CORE_INC) -Itests \
	    $(TEST_SRC) $(APP_SRC) $(CORE_SRC) $(MBEDTLS_LIBS) -lsqlite3 -lresolv -lpthread -o $@

# Black-box end-to-end integration against the containerized daemon (compose).
integration: build/e2e_client
	Scripts/test-integration.sh

# The e2e client links only the shared wire modules (no daemon internals).
build/e2e_client: tests/e2e_client.c $(SHARED_SRC) $(wildcard shared/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O0 -g -Ishared -I$(MBEDTLS_INC) \
	    tests/e2e_client.c $(SHARED_SRC) $(MBEDTLS_LIBS) -o $@

# Flexible black-box client for the local federated demo (register a device token /
# send a message against a running daemon). Same shared-only linkage as e2e_client.
demo-client: build/demo_client
build/demo_client: tests/demo_client.c $(SHARED_SRC) $(wildcard shared/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O0 -g -Ishared -I$(MBEDTLS_INC) \
	    tests/demo_client.c $(SHARED_SRC) $(MBEDTLS_LIBS) -o $@

# Capacity benchmark load client (REQ-210/211); driven by Scripts/bench.sh.
# Links only the shared wire modules, like the e2e client.
bench: build/bench_load
build/bench_load: tests/bench_load.c $(SHARED_SRC) $(wildcard shared/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O2 -Ishared -I$(MBEDTLS_INC) \
	    tests/bench_load.c $(SHARED_SRC) $(MBEDTLS_LIBS) -lpthread -o $@

# Standalone compile check for the client app-core (no frontend, no main). The
# headless test binary (make test) is the real coverage; this just proves the
# core still compiles on its own against the shared wire code.
# Manual S3 smoke test (tests/manual/s3_smoke.c) -- NOT part of `make test`.
# Needs live S3 credentials in the environment; see the header of that file.
s3-smoke: tests/manual/s3_smoke.c daemon/blobstore.c daemon/blob_s3.c daemon/sigv4.c shared/tls.c $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) $(INC) tests/manual/s3_smoke.c daemon/blobstore.c \
	    daemon/blob_s3.c daemon/sigv4.c shared/tls.c $(MBEDTLS_LIBS) -lpthread -o build/s3_smoke
	@echo "built build/s3_smoke -- needs OPENCHIME_S3_* credentials to run"

core: $(CORE_SRC) $(SHARED_SRC) $(wildcard client/core/*.h shared/*.h) $(MBEDTLS_A) | build
	mkdir -p build/core
	for f in $(CORE_SRC); do \
	    $(CC) $(CFLAGS) $(INC) $(CORE_INC) -c $$f -o build/core/$$(basename $$f .c).o || exit 1; \
	done

# The TUI: app-core + shared wire + termbox2/utf8proc. -Wno-unused-result relaxes
# one warning from the vendored termbox2 header (its read/write/strerror_r calls).
tui: $(TUI_BIN)
$(TUI_BIN): $(TUI_SRC) $(TUIKIT_SRC) $(CORE_SRC) $(SHARED_SRC) $(UTF8PROC) \
            $(wildcard client/tui/*.h tuikit/*.h client/core/*.h shared/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -Wno-unused-result $(INC) $(TUI_INC) $(TUIKIT_INC) $(SECRET_CFLAGS) \
	    $(TUI_SRC) $(TUIKIT_SRC) $(CORE_SRC) $(SHARED_SRC) $(UTF8PROC) $(MBEDTLS_LIBS) -lresolv -lpthread $(SECRET_LIBS) -o $@


# --- Windows TUI (ARCH-81) ----------------------------------------------------
# Cross-compiled with mingw-w64 to a standalone .exe. Uses the Windows mbedTLS
# (third_party/mbedtls-3.6.2-win). No sqlite: a client embeds no database engine
# (ARCH-88). The termbox2 backend is tuikit/tk_term.c (Console API); the
# core seams (threads/DNS/RNG) are in shared/oc_thread.h + resolve.c + net.c.
WINCC     ?= x86_64-w64-mingw32-gcc
WINDRES   ?= x86_64-w64-mingw32-windres
MBEDTLS_WIN := third_party/mbedtls-3.6.2-win
WIN_MBEDLIBS := $(MBEDTLS_WIN)/library/libmbedtls.a \
                $(MBEDTLS_WIN)/library/libmbedx509.a \
                $(MBEDTLS_WIN)/library/libmbedcrypto.a
WIN_TUI_BIN := build/openchime-tui.exe
WIN_CFLAGS := -std=c99 -Wall -Wextra -O2 -D_WIN32_WINNT=0x0601 -DUTF8PROC_STATIC
WIN_INC := -Ishared -Idaemon -Ithird_party/jsmn -I$(MBEDTLS_WIN)/include \
           $(CORE_INC) -Iclient/tui -Iclient/shared -Ituikit -Ithird_party/termbox2 -Ithird_party/utf8proc

windows-tui: $(WIN_TUI_BIN)
$(WIN_TUI_BIN): $(TUI_SRC) $(TUIKIT_SRC) $(CORE_SRC) $(SHARED_SRC) $(UTF8PROC) \
                $(wildcard client/tui/*.h tuikit/*.h client/core/*.h shared/*.h) $(WIN_MBEDLIBS) | build
	$(WINCC) $(WIN_CFLAGS) -Wno-unused-result $(WIN_INC) \
	    $(TUI_SRC) $(TUIKIT_SRC) $(CORE_SRC) $(SHARED_SRC) $(UTF8PROC) \
	    $(WIN_MBEDLIBS) -lws2_32 -ldnsapi -lbcrypt -lole32 -ladvapi32 -static -o $@


# The native Windows GUI (Win32 + Direct2D/DirectWrite/WIC, pure C — ARCH-80/82)
# over the same shared app-core. Mirrors windows-tui (core+shared+mbedtls-win)
# but compiles the client/gui/win32 sources instead of the
# TUI/tuikit stack and links the Direct2D stack. -municode gives the wWinMain
# Unicode entry point; -mwindows selects the GUI subsystem (no console).
WIN_GUI_BIN := build/openchime.exe
GUI_SRC := $(wildcard client/gui/win32/*.c) client/shared/icons.c client/shared/secret_win.c
WIN_GUI_INC := -Ishared -Idaemon -Ithird_party/jsmn -I$(MBEDTLS_WIN)/include \
               $(CORE_INC) -Iclient/gui/win32 -Iclient/shared

# Resources (app icon). Regenerate the .ico with scripts/gen_appicon.py.
WIN_GUI_RES := build/openchime_res.o
$(WIN_GUI_RES): client/gui/win32/res/openchime.rc client/gui/win32/res/openchime.ico \
                client/gui/win32/res/openchime_res.h | build
	$(WINDRES) -I client/gui/win32/res $< -O coff -o $@

windows-gui: $(WIN_GUI_BIN)
$(WIN_GUI_BIN): $(GUI_SRC) $(CORE_SRC) $(SHARED_SRC) $(WIN_GUI_RES) \
                $(wildcard client/gui/win32/*.h client/core/*.h shared/*.h) $(WIN_MBEDLIBS) | build
	$(WINCC) $(WIN_CFLAGS) -Wno-unused-result -municode -mwindows $(WIN_GUI_INC) -Iclient/gui/win32/res \
	    $(GUI_SRC) $(CORE_SRC) $(SHARED_SRC) $(WIN_GUI_RES) \
	    $(WIN_MBEDLIBS) -lws2_32 -ldnsapi -lbcrypt -lole32 -lshell32 -lcomdlg32 -lgdi32 -ladvapi32 \
	    -ld2d1 -ldwrite -lwindowscodecs -ldwmapi -luuid -static -o $@

# --- tuikit demo (ARCH-83) ----------------------------------------------------
# Standalone harness exercising every tuikit widget — no core, no daemon, no TLS.
# The toolbox's own smoke test.
tuikit-demo: build/tuikit-demo
build/tuikit-demo: $(TUIKIT_SRC) tuikit/demo.c $(UTF8PROC) $(wildcard tuikit/*.h) | build
	$(CC) $(CFLAGS) -Wno-unused-result $(TUIKIT_INC) -Ithird_party/termbox2 -Ithird_party/utf8proc \
	    $(TUIKIT_SRC) tuikit/demo.c $(UTF8PROC) -o $@

build:
	mkdir -p build

clean:
	rm -f $(BIN)
	rm -rf build
