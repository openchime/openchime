CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -O2
LDFLAGS ?= -lsqlite3 -lpthread

BIN := openchimed
SRC := src/main.c

TEST_BINS := build/test_protocol build/test_migrate

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

# Unit tests (docs/TESTING.md §2). Each test TU #includes the .c under test
# directly, so it needs no extra objects. Built -O0 -g for debuggability; a
# non-zero exit fails the build and CI.
test: $(TEST_BINS)
	./build/test_protocol
	./build/test_migrate

# The codec is pure — no library link needed.
build/test_protocol: tests/test_protocol.c src/protocol.c src/protocol.h | build
	$(CC) $(CFLAGS) -O0 -g -Isrc tests/test_protocol.c -o $@

# The migrations runner links SQLite.
build/test_migrate: tests/test_migrate.c src/migrate.c src/migrate.h | build
	$(CC) $(CFLAGS) -O0 -g -Isrc tests/test_migrate.c -o $@ -lsqlite3

build:
	mkdir -p build

clean:
	rm -f $(BIN)
	rm -rf build
