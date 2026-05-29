CC ?= gcc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -Wpedantic -D_GNU_SOURCE -pthread
LDFLAGS ?= -lm -pthread

BIN := build/rinha-api
CONVERTER := build/convert-references
INDEX_BUILDER := build/build-index
SRC := src/main.c
CONVERTER_SRC := src/convert-references.c
INDEX_BUILDER_SRC := src/build-index.c

.PHONY: all clean run

all: $(BIN) $(CONVERTER) $(INDEX_BUILDER)

$(BIN): $(SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(CONVERTER): $(CONVERTER_SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(CONVERTER_SRC) $(LDFLAGS)

$(INDEX_BUILDER): $(INDEX_BUILDER_SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(INDEX_BUILDER_SRC) $(LDFLAGS)

run: $(BIN)
	PORT=9999 REFERENCES_PATH=resources/example-references.json ./$(BIN)

clean:
	rm -rf build
