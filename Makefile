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

# --- Client TUI frontend (ARCH-75) --------------------------------------------
# termbox2 (cell grid + input) + utf8proc (Unicode width/grapheme), both vendored
# as committed MIT single-file source. Builds on the host like the daemon.
TUI_SRC   := $(wildcard client/tui/*.c)
UTF8PROC  := third_party/utf8proc/utf8proc.c
# The core's local store (client/core/store.c) reuses the daemon's migration
# runner and SQLite, so the frontend links migrate.c + libsqlite3.
STORE_DEPS := daemon/migrate.c
TUI_INC   := $(CORE_INC) -Iclient/tui -Ithird_party/termbox2 -Ithird_party/utf8proc
TUI_BIN   := build/openchime-tui

.PHONY: all test integration core tui bench clean

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

# The TUI: app-core + shared wire + termbox2/utf8proc. -Wno-unused-result relaxes
# one warning from the vendored termbox2 header (its read/write/strerror_r calls).
tui: $(TUI_BIN)
$(TUI_BIN): $(TUI_SRC) $(CORE_SRC) $(SHARED_SRC) $(UTF8PROC) \
            $(wildcard client/tui/*.h client/core/*.h shared/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -Wno-unused-result $(INC) $(TUI_INC) \
	    $(TUI_SRC) $(CORE_SRC) $(SHARED_SRC) $(STORE_DEPS) $(UTF8PROC) $(MBEDTLS_LIBS) -lsqlite3 -lpthread -o $@

build:
	mkdir -p build

clean:
	rm -f $(BIN)
	rm -rf build
