# Dantrix - high performance HTTP/1.1 server in C11 for Linux.
#
# Quick reference:
#   make                 build the release binary into bin/dantrix
#   make debug           unoptimised build with debug info
#   make test            build and run the unit tests
#   make asan / tsan     sanitizer builds
#   make format          rewrite sources with clang-format
#   make lint            static analysis (needs cppcheck and/or clang-tidy)
#   make clean           remove build output
#
# Overridable: CC, CFLAGS, LDFLAGS, PREFIX, DESTDIR, V=1

ifeq ($(origin CC),default)
CC := gcc
endif

PREFIX  ?= /usr/local
DESTDIR ?=

BUILD := build
BIN   := bin
SRC   := src
TESTS := tests/unit
EXAMPLES := examples

VERSION := 0.1.0

# --- Architecture specific hardening -------------------------------------
ARCH := $(shell uname -m)
ifeq ($(ARCH),x86_64)
ARCH_HARDEN := -fcf-protection
else
ARCH_HARDEN := -mbranch-protection=standard
endif

# --- Flags ----------------------------------------------------------------
STD       := -std=c11
DEFS      := -D_GNU_SOURCE
WARN      := -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wconversion \
             -Wstrict-prototypes -Wmissing-prototypes -Wnull-dereference \
             -Wundef
HARDEN_C  := -fstack-protector-strong -fstack-clash-protection $(ARCH_HARDEN) \
             -D_FORTIFY_SOURCE=2 -fPIE -U_FORTIFY_SOURCE
HARDEN_LD := -pie -Wl,-z,relro,-z,now,-z,noexecstack

REL_CFLAGS := -O2 -DNDEBUG
DBG_CFLAGS := -O0 -g3 -fno-omit-frame-pointer
PRF_CFLAGS := -O2 -g -fno-omit-frame-pointer

CFLAGS ?= $(REL_CFLAGS)
ALL_CFLAGS := $(STD) $(DEFS) $(WARN) $(HARDEN_C) $(CFLAGS) $(CFLAGS_EXTRA)
LDFLAGS ?=
ALL_LDFLAGS := $(HARDEN_LD) $(LDFLAGS)
LDLIBS ?=

# -Werror is enabled by default because the CI gate depends on it; override
# with WERROR=0 while developing.
ifeq ($(WERROR),0)
ALL_CFLAGS := $(filter-out -Werror,$(ALL_CFLAGS))
else
ALL_CFLAGS += -Werror
endif

# --- Sources --------------------------------------------------------------
LIB_SRCS := $(filter-out $(SRC)/main.c,$(wildcard $(SRC)/*.c))
LIB_OBJS := $(patsubst $(SRC)/%.c,$(BUILD)/release/%.o,$(LIB_SRCS))
MAIN_OBJ := $(BUILD)/release/main.o
DBG_OBJS := $(patsubst $(SRC)/%.c,$(BUILD)/debug/%.o,$(LIB_SRCS))
DBG_MAIN := $(BUILD)/debug/main.o

TEST_SRCS := $(wildcard $(TESTS)/test_*.c)
TEST_BINS := $(patsubst $(TESTS)/%.c,$(BUILD)/%,$(TEST_SRCS))

EX_SRC := $(wildcard $(EXAMPLES)/*.c)
EX_BINS := $(patsubst $(EXAMPLES)/%.c,$(BUILD)/%,$(EX_SRC))

.PHONY: all release debug profile asan tsan examples \
        test test-unit test-asan test-tsan test-c10k \
        lint format format-check coverage bench stress fuzz valgrind \
        install uninstall docker clean distclean help

all: release

release: $(BIN)/dantrix

debug: $(BUILD)/debug/dantrix

profile: $(BUILD)/profile/dantrix

# --- Compile rules --------------------------------------------------------
$(BUILD)/release/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(BUILD)/debug/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) $(HARDEN_C) $(DBG_CFLAGS) $(CFLAGS_EXTRA) \
	    -c $< -o $@

$(BIN)/dantrix: $(LIB_OBJS) $(MAIN_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $^ $(ALL_LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/debug/dantrix: $(DBG_OBJS) $(DBG_MAIN)
	@mkdir -p $(dir $@)
	$(CC) $^ $(HARDEN_LD) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/profile/dantrix: $(LIB_SRCS) $(SRC)/main.c
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) $(PRF_CFLAGS) $(CFLAGS_EXTRA) \
	    $^ $(HARDEN_LD) $(LDFLAGS) $(LDLIBS) -o $@

# --- Sanitizers -----------------------------------------------------------
asan: $(BUILD)/asan/dantrix

$(BUILD)/asan/dantrix: $(LIB_SRCS) $(SRC)/main.c
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) $(DBG_CFLAGS) \
	    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	    -fno-omit-frame-pointer $^ -o $@ $(LDFLAGS) $(LDLIBS)

tsan: $(BUILD)/tsan/dantrix

$(BUILD)/tsan/dantrix: $(LIB_SRCS) $(SRC)/main.c
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) $(DBG_CFLAGS) -fsanitize=thread \
	    -fno-omit-frame-pointer $^ -o $@ $(LDFLAGS) $(LDLIBS)

# --- Examples -------------------------------------------------------------
examples: $(EX_BINS)

$(BUILD)/%: $(EXAMPLES)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) -O2 $(CFLAGS_EXTRA) $< -o $@ $(LDFLAGS) -lpthread

# --- Tests ----------------------------------------------------------------
$(BUILD)/test_%: $(TESTS)/test_%.c $(LIB_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) -O1 -g $(CFLAGS_EXTRA) \
	    -I$(SRC) -I$(TESTS) $^ -o $@ $(LDFLAGS) -lpthread

test: test-unit

test-unit: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do \
	    echo "== $$t"; ./$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "unit tests FAILED"; exit 1; fi; \
	echo "all unit tests passed"

$(BUILD)/test_%_asan: $(TESTS)/test_%.c $(LIB_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) -O1 -g \
	    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	    -I$(SRC) -I$(TESTS) $^ -o $@ $(LDFLAGS) -lpthread

test-asan:
	@fail=0; \
	for src in $(TEST_SRCS); do \
	    name=$$(basename $$src .c); \
	    $(MAKE) --no-print-directory $(BUILD)/$${name#test_}_asan_bin; \
	done; \
	echo "build ASan test binaries with: make test-asan"

$(BUILD)/%_asan_bin: $(TESTS)/test_%.c $(LIB_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) -O1 -g \
	    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	    -I$(SRC) -I$(TESTS) $^ -o $@ $(LDFLAGS) -lpthread

test-tsan:
	@fail=0; \
	for src in $(TEST_SRCS); do \
	    name=$$(basename $$src .c); \
	    $(CC) $(STD) $(DEFS) $(WARN) -O1 -g -fsanitize=thread \
	        -I$(SRC) -I$(TESTS) $(SRC)/util.c $(SRC)/log.c \
	        $(TESTS)/$$src -o $(BUILD)/$${name#test_}_tsan -lpthread || exit 1; \
	done; \
	echo "build TSan test binaries with the command above"

valgrind:
	@command -v valgrind >/dev/null 2>&1 || { \
	    echo "valgrind not installed; skipping"; exit 0; }
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 \
	    $(BIN)/dantrix --help

# --- Quality --------------------------------------------------------------
lint:
	@if command -v cppcheck >/dev/null 2>&1; then \
	    cppcheck --enable=warning,style,performance,portability \
	        --std=c11 --error-exitcode=1 --quiet \
	        --suppress=normalCheckLevelMaxBranches \
	        -I $(SRC) $(SRC); \
	    echo "cppcheck: clean"; \
	else echo "cppcheck not installed; skipped"; fi
	@if command -v clang-tidy >/dev/null 2>&1; then \
	    echo "clang-tidy available; run it with compile_commands.json (bear -- make debug)"; \
	else echo "clang-tidy not installed; skipped"; fi

format:
	@if command -v clang-format >/dev/null 2>&1; then \
	    clang-format -i $(SRC)/*.c $(SRC)/*.h $(TESTS)/*.c $(EXAMPLES)/*.c; \
	    echo "sources formatted"; \
	else echo "clang-format not installed; skipped"; fi

format-check:
	@if command -v clang-format >/dev/null 2>&1; then \
	    clang-format --dry-run --Werror $(SRC)/*.c $(SRC)/*.h && \
	    echo "format check: clean"; \
	else echo "clang-format not installed; skipped"; fi

coverage:
	@mkdir -p coverage
	@fail=0; \
	for src in $(TEST_SRCS); do \
	    name=$$(basename $$src .c); \
	    $(CC) $(STD) $(DEFS) $(WARN) -O0 -g --coverage \
	        -I$(SRC) -I$(TESTS) $(LIB_SRCS) $(TESTS)/$$src \
	        -o $(BUILD)/$${name#test_}_cov || exit 1; \
	    ./$(BUILD)/$${name#test_}_cov || fail=1; \
	done; \
	lcov --capture --directory . --output-file coverage.info >/dev/null 2>&1 || true; \
	lcov --list coverage.info 2>/dev/null | tail -n 20 || true; \
	if [ $$fail -ne 0 ]; then exit 1; fi

# --- Benchmarking, fuzzing and deployment (later milestones) -------------
bench:
	@echo "benchmark scripts land in v0.7; nothing to run yet"

stress:
	@echo "stress harness lands in v0.7; nothing to run yet"

# C10K check: hold N idle connections open at once. N defaults to 10000 and
# can be overridden: make test-c10k N=2000
N ?= 10000

test-c10k: release
	@python3 tests/c10k_test.py $(N) $(BIN)/dantrix

# Fuzzers. Without clang/libFuzzer the standalone deterministic drivers run
# the same invariant checks with plain gcc.
FUZZ_PARSER_SRCS := src/http_parser.c src/http_response.c src/util.c
FUZZ_PATH_SRCS := src/path.c src/http_range.c src/mime.c

$(BUILD)/fuzz_http_parser: fuzz/fuzz_http_parser.c $(FUZZ_PARSER_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) -O1 -g -DFUZZ_STANDALONE \
	    -I$(SRC) $< $(FUZZ_PARSER_SRCS) -o $@ $(LDFLAGS)

$(BUILD)/fuzz_path: fuzz/fuzz_path.c $(FUZZ_PATH_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) -O1 -g -DFUZZ_STANDALONE \
	    -I$(SRC) $< $(FUZZ_PATH_SRCS) -o $@ $(LDFLAGS)

fuzz: $(BUILD)/fuzz_http_parser $(BUILD)/fuzz_path
	./$(BUILD)/fuzz_http_parser
	./$(BUILD)/fuzz_path

docker:
	docker build -f deploy/Dockerfile -t dantrix:latest .

install: release
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN)/dantrix $(DESTDIR)$(PREFIX)/bin/dantrix
	install -d $(DESTDIR)$(PREFIX)/share/dantrix
	install -m 0644 conf/dantrix.conf $(DESTDIR)$(PREFIX)/share/dantrix/dantrix.conf.example
	install -m 0644 conf/mime.types $(DESTDIR)$(PREFIX)/share/dantrix/mime.types

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dantrix
	rm -rf $(DESTDIR)$(PREFIX)/share/dantrix

clean:
	rm -rf $(BUILD) $(BIN) coverage coverage.info

distclean: clean

help:
	@sed -n '1,12p' Makefile
