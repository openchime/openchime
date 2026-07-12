CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -O2
LDFLAGS ?= -lsqlite3 -lpthread

BIN := openchimed
SRC := src/main.c

TEST_BIN := build/test_protocol

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

# Unit tests (docs/TESTING.md §2). The test TU #includes the .c under test
# directly, so it needs no extra objects and no sqlite/pthread link. Built
# -O0 -g for debuggability; a non-zero exit fails the build and CI.
test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): tests/test_protocol.c src/protocol.c src/protocol.h | build
	$(CC) $(CFLAGS) -O0 -g -Isrc tests/test_protocol.c -o $@

build:
	mkdir -p build

clean:
	rm -f $(BIN)
	rm -rf build
