CC ?= gcc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -Wpedantic -D_GNU_SOURCE -pthread
LDFLAGS ?= -lm -pthread

BIN := build/rinha-api
LB := build/rinha-lb
CONVERTER := build/convert-references
INDEX_BUILDER := build/build-index
SRC := src/main.c
LB_SRC := src/lb-id.c
CONVERTER_SRC := src/convert-references.c
INDEX_BUILDER_SRC := src/build-index.c

.PHONY: all clean run

all: $(BIN) $(LB) $(CONVERTER) $(INDEX_BUILDER)

$(BIN): $(SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(LB): $(LB_SRC) src/known_ids.inc
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(LB_SRC) -pthread

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
