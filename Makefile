CC ?= gcc
PYTHON ?= python3

CPPFLAGS := -Iinclude -D_GNU_SOURCE
COMMON_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -pthread
RELEASE_CFLAGS := -O3 -DNDEBUG
DEBUG_CFLAGS := -O0 -g -DDEBUG
LDFLAGS := -pthread

SRCS := $(shell find src -type f -name '*.c' | sort)
RELEASE_OBJS := $(patsubst src/%.c,build/release/%.o,$(SRCS))
DEBUG_OBJS := $(patsubst src/%.c,build/debug/%.o,$(SRCS))
UNAME_S := $(shell uname -s)

.PHONY: all release debug unit integration test bench clean

all: release

release: httpd

debug: httpd-debug

httpd: $(RELEASE_OBJS)
	$(CC) $(COMMON_CFLAGS) $(RELEASE_CFLAGS) $^ -o $@ $(LDFLAGS)

httpd-debug: $(DEBUG_OBJS)
	$(CC) $(COMMON_CFLAGS) $(DEBUG_CFLAGS) $^ -o $@ $(LDFLAGS)

build/release/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(COMMON_CFLAGS) $(RELEASE_CFLAGS) -c $< -o $@

build/debug/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(COMMON_CFLAGS) $(DEBUG_CFLAGS) -c $< -o $@

parser_tests: tests/parser_tests.c src/http/parser.c src/util/util.c include/http_parser.h include/util.h
	$(CC) $(CPPFLAGS) $(COMMON_CFLAGS) $(DEBUG_CFLAGS) tests/parser_tests.c src/http/parser.c src/util/util.c -o $@ $(LDFLAGS)

unit: parser_tests
	./parser_tests

ifeq ($(UNAME_S),Linux)
integration: httpd-debug
	$(PYTHON) tests/integration_test.py --httpd ./httpd-debug
else
integration:
	@echo "integration test skipped (requires Linux epoll runtime)"
endif

test: unit integration

bench: httpd
	bash tests/benchmark.sh

clean:
	rm -rf build httpd httpd-debug parser_tests
