CC ?= gcc
AR ?= ar
PYTHON ?= python3
BUILD ?= build
CORPUS ?= corpus
OUTPUT ?= runs/benchmark
CPU ?= 0
ROUNDS ?= 5
SECONDS ?= 1.5

CFLAGS ?= -O3 -std=c11 -Wall -Wextra -Werror -flto -fno-plt
ARCHFLAGS = -march=armv8-a+crc
CPPFLAGS += -Iinclude
CODEC = src/encoder.c src/decoder.c
HEADERS = include/codec.h src/numeric_encode.inc src/numeric_decode.inc
OBJECTS = $(BUILD)/encoder.o $(BUILD)/decoder.o
LIB = $(BUILD)/libfastpage.so

.PHONY: all example test sanitize verify bench fetch clean help

all: $(LIB) $(BUILD)/libfastpage.a $(BUILD)/benchmark

$(BUILD):
	mkdir -p "$@"

$(BUILD)/%.o: src/%.c $(HEADERS) Makefile | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ARCHFLAGS) -fPIC -c $< -o $@

$(LIB): $(OBJECTS) Makefile
	$(CC) $(CFLAGS) $(ARCHFLAGS) $(LDFLAGS) -shared $(OBJECTS) -o $@

$(BUILD)/libfastpage.a: $(OBJECTS) Makefile
	$(AR) rcs $@ $(OBJECTS)

$(BUILD)/benchmark: bench/benchmark.c Makefile | $(BUILD)
	$(CC) -O3 -std=c11 -Wall -Wextra -Werror $< -o $@ -ldl -lm

$(BUILD)/example: examples/roundtrip.c include/codec.h $(LIB) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ARCHFLAGS) $< -L$(BUILD) -lfastpage $(LDFLAGS) -Wl,-rpath,'$$ORIGIN' -o $@

example: $(BUILD)/example
	./$(BUILD)/example

$(BUILD)/verify_vectors: tests/verify_vectors.c $(LIB) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ARCHFLAGS) $< -L$(BUILD) -lfastpage $(LDFLAGS) -Wl,-rpath,'$$ORIGIN' -o $@

$(BUILD)/verify_roundtrip: tests/verify_roundtrip.c include/codec.h $(LIB) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ARCHFLAGS) $< -L$(BUILD) -lfastpage $(LDFLAGS) -Wl,-rpath,'$$ORIGIN' -o $@

$(BUILD)/verify_guards: tests/verify_guards.c Makefile | $(BUILD)
	$(CC) $(CFLAGS) $(ARCHFLAGS) $< $(LDFLAGS) -pthread -ldl -o $@

test: $(BUILD)/verify_vectors $(BUILD)/verify_roundtrip $(BUILD)/verify_guards $(LIB)
	./$(BUILD)/verify_vectors
	./$(BUILD)/verify_roundtrip
	./$(BUILD)/verify_guards ./$(LIB)

sanitize:
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) BUILD=build/sanitize CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test

verify:
	$(PYTHON) tools/benchmark.py --corpus "$(CORPUS)" --cpu $(CPU) --verify-only --output "$(OUTPUT)"

bench:
	$(PYTHON) tools/benchmark.py --corpus "$(CORPUS)" --cpu $(CPU) --rounds $(ROUNDS) --seconds $(SECONDS) --output "$(OUTPUT)"

fetch:
	$(PYTHON) tools/fetch_corpus.py "$(CORPUS)"

clean:
	rm -rf build

help:
	@echo 'all / example / test / sanitize / fetch / verify / bench / clean'
	@echo 'bench: CORPUS=path CPU=0 ROUNDS=5 SECONDS=1.5 OUTPUT=runs/name'
