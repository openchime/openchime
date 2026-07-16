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

# --- Client app-core (ARCH-74) ------------------------------------------------
# The shared, frontend-agnostic core: the net thread + the two UI<->net queues +
# the view-model/reducers + the oc_client facade. It has no main of its own — the
# headless test (make test) links and drives it against an in-process daemon, and
# frontends (a TUI first, then native GUIs) link it too. See docs/CLIENT.md.
CORE_SRC := $(wildcard client/core/*.c)
CORE_INC := -Iclient/core

.PHONY: all test integration core bench clean

all: $(BIN)

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
	    $(TEST_SRC) $(APP_SRC) $(CORE_SRC) $(MBEDTLS_LIBS) -lsqlite3 -lpthread -o $@

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

# Standalone compile check for the client app-core (no frontend, no main). The
# headless test binary (make test) is the real coverage; this just proves the
# core still compiles on its own against the shared wire code.
core: $(CORE_SRC) $(SHARED_SRC) $(wildcard client/core/*.h shared/*.h) $(MBEDTLS_A) | build
	mkdir -p build/core
	for f in $(CORE_SRC); do \
	    $(CC) $(CFLAGS) $(INC) $(CORE_INC) -c $$f -o build/core/$$(basename $$f .c).o || exit 1; \
	done

build:
	mkdir -p build

clean:
	rm -f $(BIN)
	rm -rf build
