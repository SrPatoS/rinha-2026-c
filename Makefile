CC ?= gcc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -Wpedantic -D_GNU_SOURCE -pthread
LDFLAGS ?= -lm -pthread

BIN := build/rinha-api
CONVERTER := build/convert-references
SRC := src/main.c
CONVERTER_SRC := src/convert-references.c

.PHONY: all clean run

all: $(BIN) $(CONVERTER)

$(BIN): $(SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(CONVERTER): $(CONVERTER_SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(CONVERTER_SRC) $(LDFLAGS)

run: $(BIN)
	PORT=9999 REFERENCES_PATH=resources/example-references.json ./$(BIN)

clean:
	rm -rf build
