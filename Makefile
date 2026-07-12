CC ?= cc
CFLAGS ?= -std=c99 -D_GNU_SOURCE -Wall -Wextra -O2
LDFLAGS ?= -lsqlite3 -lpthread

BIN := openchimed
# The daemon links every module; each .c is a separate TU (the tests instead
# #include them directly, openblocks-style).
SRC := src/main.c src/protocol.c src/migrate.c src/framebuf.c \
       src/dbwriter.c src/tls.c src/netloop.c

# Vendored, pinned mbedTLS (scripts/build_mbedtls.sh) — one version across
# local/CI/Docker. Link order matters for static archives: tls -> x509 -> crypto.
MBEDTLS_DIR  := third_party/mbedtls-3.6.2
MBEDTLS_INC  := $(MBEDTLS_DIR)/include
MBEDTLS_A    := $(MBEDTLS_DIR)/library/libmbedtls.a
MBEDTLS_LIBS := $(MBEDTLS_DIR)/library/libmbedtls.a \
                $(MBEDTLS_DIR)/library/libmbedx509.a \
                $(MBEDTLS_DIR)/library/libmbedcrypto.a

# Every source except the daemon entry point; the test binary links these and
# calls their public APIs (no per-test binaries, no unity #include of .c).
APP_SRC   := $(filter-out src/main.c,$(SRC))
# e2e_client is a standalone black-box tool (its own main), not part of the
# single in-process test binary.
TEST_SRC  := $(filter-out tests/e2e_client.c,$(wildcard tests/*.c))
TEST_BIN  := build/tests

.PHONY: all test integration clean

all: $(BIN)

$(BIN): $(SRC) $(MBEDTLS_A) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -I$(MBEDTLS_INC) -o $@ $(SRC) $(MBEDTLS_LIBS) $(LDFLAGS)

$(MBEDTLS_A):
	scripts/build_mbedtls.sh

# Unit + in-process integration tests, one binary (docs/TESTING.md §2). Built
# -O0 -g; a non-zero exit fails the build and CI.
test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(APP_SRC) $(wildcard src/*.h) $(wildcard tests/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O0 -g -Isrc -Itests -I$(MBEDTLS_INC) \
	    $(TEST_SRC) $(APP_SRC) $(MBEDTLS_LIBS) -lsqlite3 -lpthread -o $@

# Black-box end-to-end integration against the containerized daemon (compose).
integration: build/e2e_client
	Scripts/test-integration.sh

# The e2e client links only the protocol + TLS modules (no daemon internals).
build/e2e_client: tests/e2e_client.c src/protocol.c src/framebuf.c src/tls.c $(wildcard src/*.h) $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O0 -g -Isrc -I$(MBEDTLS_INC) \
	    tests/e2e_client.c src/protocol.c src/framebuf.c src/tls.c $(MBEDTLS_LIBS) -o $@

build:
	mkdir -p build

clean:
	rm -f $(BIN)
	rm -rf build
