CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -O2
LDFLAGS ?= -lsqlite3 -lpthread

BIN := openchimed
SRC := src/main.c

# Vendored, pinned mbedTLS (scripts/build_mbedtls.sh) — one version across
# local/CI/Docker. Link order matters for static archives: tls -> x509 -> crypto.
MBEDTLS_DIR  := third_party/mbedtls-3.6.2
MBEDTLS_INC  := $(MBEDTLS_DIR)/include
MBEDTLS_A    := $(MBEDTLS_DIR)/library/libmbedtls.a
MBEDTLS_LIBS := $(MBEDTLS_DIR)/library/libmbedtls.a \
                $(MBEDTLS_DIR)/library/libmbedx509.a \
                $(MBEDTLS_DIR)/library/libmbedcrypto.a

TEST_BINS := build/test_protocol build/test_migrate build/itest_tls

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(MBEDTLS_A):
	scripts/build_mbedtls.sh

# Unit/integration tests (docs/TESTING.md §2). Each test TU #includes the .c
# under test directly, so it needs no extra objects. Built -O0 -g; a non-zero
# exit fails the build and CI.
test: $(TEST_BINS)
	./build/test_protocol
	./build/test_migrate
	./build/itest_tls

# The codec is pure — no library link needed.
build/test_protocol: tests/test_protocol.c src/protocol.c src/protocol.h | build
	$(CC) $(CFLAGS) -O0 -g -Isrc tests/test_protocol.c -o $@

# The migrations runner links SQLite.
build/test_migrate: tests/test_migrate.c src/migrate.c src/migrate.h | build
	$(CC) $(CFLAGS) -O0 -g -Isrc tests/test_migrate.c -o $@ -lsqlite3

# The TLS wrapper links vendored mbedTLS + pthread.
build/itest_tls: tests/itest_tls.c src/tls.c src/tls.h $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O0 -g -Isrc -I$(MBEDTLS_INC) tests/itest_tls.c $(MBEDTLS_LIBS) -lpthread -o $@

build:
	mkdir -p build

clean:
	rm -f $(BIN)
	rm -rf build
