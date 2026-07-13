CC ?= cc
CFLAGS ?= -std=c99 -D_GNU_SOURCE -Wall -Wextra -O2
LDFLAGS ?= -lsqlite3 -lpthread

BIN := openchimed

# The tree is split into three concerns: shared/ is the wire contract (linked by
# both the daemon and the client), daemon/ is the server, client/ is the app.
SHARED_SRC := shared/protocol.c shared/framebuf.c shared/tls.c
DAEMON_SRC := daemon/main.c daemon/migrate.c daemon/dbwriter.c daemon/netloop.c
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

INC := -Ishared -Idaemon -I$(MBEDTLS_INC)

# Every source except the daemon entry point; the test binary links these and
# calls their public APIs (no per-test binaries, no unity #include of .c).
APP_SRC   := $(SHARED_SRC) $(filter-out daemon/main.c,$(DAEMON_SRC))
# e2e_client is a standalone black-box tool (its own main), not part of the
# single in-process test binary.
TEST_SRC  := $(filter-out tests/e2e_client.c,$(wildcard tests/*.c))
TEST_BIN  := build/tests

# --- Client (raylib GUI; built in a container, see Dockerfile.client) ---------
RAYLIB_DIR  := third_party/raylib-install
CLIENT_SRC  := $(wildcard client/*.c)
CLIENT_BIN  := build/openchime-client
# Static raylib pulls in the GL/X11 stack at link time.
CLIENT_LIBS := $(RAYLIB_DIR)/lib/libraylib.a -lGL -lm -lpthread -ldl -lrt \
               -lX11 -lXrandr -lXinerama -lXcursor -lXi

.PHONY: all test integration client clean

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

client: $(CLIENT_BIN)

$(CLIENT_BIN): $(CLIENT_SRC) $(SHARED_SRC) $(wildcard client/*.h shared/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -Ishared -Iclient -I$(RAYLIB_DIR)/include -I$(MBEDTLS_INC) \
	    $(CLIENT_SRC) $(SHARED_SRC) $(MBEDTLS_LIBS) $(CLIENT_LIBS) -o $@

build:
	mkdir -p build

clean:
	rm -f $(BIN)
	rm -rf build

# The client is a GUI app (raylib) built in a container so the X11/GL toolchain
# stays off the host — see Dockerfile.client and docs/CLIENT.md.
