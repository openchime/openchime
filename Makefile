CC ?= cc
CFLAGS ?= -std=c99 -D_GNU_SOURCE -Wall -Wextra -O2
LDFLAGS ?= -lsqlite3 -lpthread

BIN := openchimed

# The tree is split into three concerns: shared/ is the wire contract (linked by
# both the daemon and the client), daemon/ is the server, client/ is the app.
SHARED_SRC := shared/protocol.c shared/framebuf.c shared/tls.c
DAEMON_SRC := daemon/main.c daemon/migrate.c daemon/dbwriter.c daemon/netloop.c daemon/auth.c daemon/jwt.c daemon/ratelimit.c daemon/roles.c daemon/blobstore.c daemon/blob_s3.c daemon/sigv4.c daemon/http.c daemon/audio_sidecar.c
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
TEST_SRC  := $(filter-out tests/e2e_client.c tests/bench_load.c,$(wildcard tests/*.c))
TEST_BIN  := build/tests

# --- Client (raylib GUI) ------------------------------------------------------
# This iteration targets Windows only (cross-compiled with mingw-w64, the
# openblocks `make windows` pattern — runs on Linux/CI, no Windows machine).
# Linux/macOS/wasm/Android/iOS are added later. See docs/CLIENT.md.
CLIENT_SRC  := $(wildcard client/*.c)

MINGW_CC     := x86_64-w64-mingw32-gcc
RAYLIB_WIN   := third_party/raylib-install-win
MBEDTLS_WIN  := third_party/mbedtls-3.6.2-win
RAYLIB_WIN_A := $(RAYLIB_WIN)/lib/libraylib.a
MBEDTLS_WIN_A := $(MBEDTLS_WIN)/library/libmbedtls.a
MBEDTLS_WIN_LIBS := $(MBEDTLS_WIN)/library/libmbedtls.a \
                    $(MBEDTLS_WIN)/library/libmbedx509.a \
                    $(MBEDTLS_WIN)/library/libmbedcrypto.a
WIN_BIN      := build/openchime-client.exe
# -std=gnu99 exposes strdup/getaddrinfo on mingw; _WIN32_WINNT>=0x0600 for WSAPoll.
WIN_CFLAGS   := -std=gnu99 -Wall -Wextra -O2 -D_WIN32_WINNT=0x0601
# Static raylib on Windows pulls in the GDI/GL/multimedia libs; the protocol
# needs winsock; mbedTLS's entropy uses BCryptGenRandom (-lbcrypt); -static makes
# a standalone .exe (bundles libgcc/winpthread).
WIN_SYS_LIBS := -lopengl32 -lgdi32 -lwinmm -lws2_32 -lbcrypt

.PHONY: all test integration windows bench clean

all: $(BIN)

$(BIN): $(SRC) $(MBEDTLS_A) $(HDRS)
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC) $(MBEDTLS_LIBS) $(LDFLAGS)

$(MBEDTLS_A):
	scripts/build_mbedtls.sh

# Unit + in-process integration tests, one binary (docs/TESTING.md §2). Built
# -O0 -g; a non-zero exit fails the build and CI.
test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(APP_SRC) $(HDRS) $(wildcard tests/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O0 -g $(INC) -Itests \
	    $(TEST_SRC) $(APP_SRC) $(MBEDTLS_LIBS) -lsqlite3 -lpthread -o $@

# Black-box end-to-end integration against the containerized daemon (compose).
integration: build/e2e_client
	Scripts/test-integration.sh

# The e2e client links only the shared wire modules (no daemon internals).
build/e2e_client: tests/e2e_client.c $(SHARED_SRC) $(wildcard shared/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O0 -g -Ishared -I$(MBEDTLS_INC) \
	    tests/e2e_client.c $(SHARED_SRC) $(MBEDTLS_LIBS) -o $@

# Capacity benchmark load client (REQ-210/211); driven by Scripts/bench.sh.
# Links only the shared wire modules, like the e2e client.
bench: build/bench_load
build/bench_load: tests/bench_load.c $(SHARED_SRC) $(wildcard shared/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O2 -Ishared -I$(MBEDTLS_INC) \
	    tests/bench_load.c $(SHARED_SRC) $(MBEDTLS_LIBS) -lpthread -o $@

# Cross-compile the Windows client (.exe). Vendors raylib + mbedTLS for Windows
# on demand, then links client/ + shared/ wire code statically.
windows: $(WIN_BIN)

$(WIN_BIN): $(CLIENT_SRC) $(SHARED_SRC) $(wildcard client/*.h shared/*.h) $(RAYLIB_WIN_A) $(MBEDTLS_WIN_A) | build
	$(MINGW_CC) $(WIN_CFLAGS) -Ishared -Iclient -I$(RAYLIB_WIN)/include -I$(MBEDTLS_WIN)/include \
	    $(CLIENT_SRC) $(SHARED_SRC) $(RAYLIB_WIN_A) $(MBEDTLS_WIN_LIBS) \
	    $(WIN_SYS_LIBS) -lpthread -static -mwindows -o $@

$(RAYLIB_WIN_A):
	scripts/build_raylib_windows.sh

$(MBEDTLS_WIN_A):
	scripts/build_mbedtls_windows.sh

build:
	mkdir -p build

clean:
	rm -f $(BIN)
	rm -rf build

# The client is a GUI app (raylib) built in a container so the X11/GL toolchain
# stays off the host — see Dockerfile.client and docs/CLIENT.md.
