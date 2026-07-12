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

TEST_BINS := build/test_protocol build/test_migrate build/test_framebuf \
             build/test_dbwriter build/itest_tls build/itest_netloop

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRC) $(MBEDTLS_A) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -I$(MBEDTLS_INC) -o $@ $(SRC) $(MBEDTLS_LIBS) $(LDFLAGS)

$(MBEDTLS_A):
	scripts/build_mbedtls.sh

# Unit/integration tests (docs/TESTING.md §2). Each test TU #includes the .c
# under test directly. Built -O0 -g; a non-zero exit fails the build and CI.
test: $(TEST_BINS)
	./build/test_protocol
	./build/test_migrate
	./build/test_framebuf
	./build/test_dbwriter
	./build/itest_tls
	./build/itest_netloop

# Pure modules — no library link needed.
build/test_protocol: tests/test_protocol.c src/protocol.c src/protocol.h | build
	$(CC) $(CFLAGS) -O0 -g -Isrc tests/test_protocol.c -o $@

build/test_framebuf: tests/test_framebuf.c src/framebuf.c src/protocol.c src/*.h | build
	$(CC) $(CFLAGS) -O0 -g -Isrc tests/test_framebuf.c -o $@

# Modules linking SQLite.
build/test_migrate: tests/test_migrate.c src/migrate.c src/migrate.h | build
	$(CC) $(CFLAGS) -O0 -g -Isrc tests/test_migrate.c -o $@ -lsqlite3

build/test_dbwriter: tests/test_dbwriter.c src/dbwriter.c src/migrate.c src/*.h | build
	$(CC) $(CFLAGS) -O0 -g -Isrc tests/test_dbwriter.c -o $@ -lsqlite3 -lpthread

# Modules linking vendored mbedTLS (+ pthread for the threaded harnesses).
build/itest_tls: tests/itest_tls.c src/tls.c src/tls.h $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O0 -g -Isrc -I$(MBEDTLS_INC) tests/itest_tls.c $(MBEDTLS_LIBS) -lpthread -o $@

build/itest_netloop: tests/itest_netloop.c src/netloop.c src/framebuf.c src/protocol.c src/tls.c src/dbwriter.c src/migrate.c src/*.h $(MBEDTLS_A) | build
	$(CC) $(CFLAGS) -O0 -g -Isrc -I$(MBEDTLS_INC) tests/itest_netloop.c $(MBEDTLS_LIBS) -lsqlite3 -lpthread -o $@

build:
	mkdir -p build

clean:
	rm -f $(BIN)
	rm -rf build
