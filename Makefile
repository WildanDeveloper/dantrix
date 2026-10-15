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

VERSION := 0.8.17

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
             -D_FORTIFY_SOURCE=2 -fPIE
HARDEN_LD := -pie -Wl,-z,relro,-z,now,-z,noexecstack

REL_CFLAGS := -O2 -DNDEBUG
DBG_CFLAGS := -O0 -g3 -fno-omit-frame-pointer
PRF_CFLAGS := -O2 -g -fno-omit-frame-pointer

# An exported CFLAGS replaces the optimisation level silently (a packaging
# default of "-g -O0" would ship a debug build as a release). Append REL_CFLAGS
# so the release profile always applies on top of whatever was asked for.
CFLAGS ?=
ALL_CFLAGS := $(STD) $(DEFS) $(WARN) $(HARDEN_C) $(CFLAGS) $(REL_CFLAGS) $(CFLAGS_EXTRA)
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
        test test-unit test-asan test-tsan test-regression test-c10k test-seccomp test-security test-toctou test-config soak \
        lint format format-check coverage bench stress fuzz fuzz-long fuzz-replay valgrind \
        install uninstall docker clean distclean help

all: release

release: $(BIN)/dantrix

debug: $(BUILD)/debug/dantrix

profile: $(BUILD)/profile/dantrix

# --- Compile rules --------------------------------------------------------
# -MMD -MP records the headers each object depends on. Without it a changed
# header left stale objects in place, and the mismatched struct layouts showed
# up as wrong counters at runtime instead of a rebuild.
DEPFLAGS := -MMD -MP
DEPS := $(LIB_OBJS:.o=.d) $(MAIN_OBJ:.o=.d) $(DBG_OBJS:.o=.d) $(DBG_MAIN:.o=.d)
DEPS += $(foreach e,$(notdir $(EX_BINS:.o=)),$(BUILD)/$(e).d)

$(BUILD)/release/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/debug/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) $(HARDEN_C) $(DBG_CFLAGS) $(CFLAGS_EXTRA) \
	    $(DEPFLAGS) -c $< -o $@

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
	$(CC) $(STD) $(DEFS) $(WARN) -O2 $(CFLAGS_EXTRA) $(DEPFLAGS) -c $< -o $@.o
	$(CC) $@.o -o $@ $(LDFLAGS) -lpthread
	@rm -f $@.o

# --- Tests ----------------------------------------------------------------
HDRS := $(wildcard $(SRC)/*.h)

# \$(HDRS) is a prerequisite only: it must not reach the compiler command line,
# or the driver sees several inputs and rejects -o.
$(BUILD)/test_%: $(TESTS)/test_%.c $(LIB_SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) -O1 -g $(CFLAGS_EXTRA) \
	    -I$(SRC) -I$(TESTS) $(filter %.c,$^) -o $@ $(LDFLAGS) -lpthread

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
	@mkdir -p $(BUILD)
	@fail=0; \
	for src in $(TEST_SRCS); do \
	    name=$$(basename $$src .c); \
	    $(MAKE) --no-print-directory $(BUILD)/$${name#test_}_asan_bin || fail=1; \
	    if [ $$fail -eq 0 ]; then \
	        echo "== ASan: build/$${name#test_}_asan_bin"; \
	        ./$(BUILD)/$${name#test_}_asan_bin || fail=1; \
	    fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo "asan unit tests FAILED"; exit 1; fi; \
	echo "all asan unit tests passed"

$(BUILD)/%_asan_bin: $(TESTS)/test_%.c $(LIB_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) -O1 -g \
	    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	    -I$(SRC) -I$(TESTS) $^ -o $@ $(LDFLAGS) -lpthread

test-tsan: tsan
	@mkdir -p $(BUILD)/tsan
	@fail=0; \
	for src in $(TEST_SRCS); do \
	    name=$$(basename $$src .c); \
	    echo "== build/tsan/$${name#test_}"; \
	    $(CC) $(STD) $(DEFS) $(WARN) -O1 -g -fsanitize=thread \
	        -I$(SRC) -I$(TESTS) $(LIB_SRCS) $$src \
	        -o $(BUILD)/tsan/$${name#test_} -lpthread || fail=1; \
	    if [ $$fail -eq 0 ]; then ./$(BUILD)/tsan/$${name#test_} || fail=1; fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo "tsan unit tests FAILED"; exit 1; fi; \
	echo "all tsan unit tests passed"
	@echo "== tests/tsan_test.py (multi-worker concurrency)"
	@python3 tests/tsan_test.py $(BUILD)/tsan/dantrix

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
	        -I$(SRC) -I$(TESTS) $(LIB_SRCS) $$src \
	        -o $(BUILD)/$${name#test_}_cov || exit 1; \
	    ./$(BUILD)/$${name#test_}_cov || fail=1; \
	done; \
	lcov --capture --directory . --output-file coverage.info >/dev/null 2>&1 || true; \
	lcov --list coverage.info 2>/dev/null | tail -n 20 || true; \
	if [ $$fail -ne 0 ]; then exit 1; fi

# --- Benchmarking, fuzzing and deployment (later milestones) -------------
bench:
	@bash bench/make_files.sh
	@echo
	@echo "Payloads ready in bench/www. Start the server and run:"
	@echo "  ./bin/dantrix -r bench/www -p 8080 -w 4 --access-log off"
	@echo "  bench/run_all.sh"
	@echo "  bench/compare_reference.sh 8080 8081"
	@echo
	@echo "Or run one scenario at a time with bench/run_wrk.sh."

stress: release
	@echo "stress: running the soak test (the previous version of this target"
	@echo "        only printed instructions, so the roadmap claim it exists to"
	@echo "        verify could never fail)"
	@python3 tests/soak_test.py --seconds $(SOAK_SECONDS) --clients $(SOAK_CLIENTS) \
	    --active $(SOAK_ACTIVE) --rate $(SOAK_RATE) $(BIN)/dantrix

# C10K check: hold N idle connections open at once. N defaults to 10000 and
# can be overridden: make test-c10k N=2000
N ?= 10000

test-regression:
	@python3 tests/regression_test.py $(BIN)/dantrix

test-c10k: release
	@python3 tests/c10k_test.py $(N) $(BIN)/dantrix

test-seccomp: release
	@python3 tests/seccomp_test.py $(BIN)/dantrix

# Reads the configuration surface the way an operator does: documented keys must
# be registered, a no-op key must say so, a rejected config must leave nothing
# behind. This is the suite that catches documentation drifting away from the
# binary (it found the stale add_header and edge_triggered claims).
test-config: release
	@python3 tests/config_test.py $(BIN)/dantrix

test-security: release
	@python3 tests/security_test.py $(BIN)/dantrix

# Races the filesystem against the server to try to falsify the atomic
# document-root claim (see SECURITY.md). Runs for 10 seconds by default;
# TOCTOU_SECONDS extends it.
test-toctou: release
	@python3 tests/toctou_test.py $(BIN)/dantrix

# The full one-hour soak the roadmap's definition of done asks for. This is the
# same test `make stress` runs with a shorter default.
soak: release
	@python3 tests/soak_test.py --seconds 3600 --clients 200 --active 4 --rate 50 \
	    $(BIN)/dantrix

# Fuzzers. Without clang/libFuzzer the standalone deterministic drivers run
# the same invariant checks with plain gcc.
FUZZ_PARSER_SRCS := src/http_parser.c src/http_response.c src/util.c
FUZZ_PATH_SRCS := src/path.c src/http_range.c src/mime.c
FUZZ_ESCAPE_SRCS := src/file.c src/path.c src/http_range.c src/mime.c src/util.c src/fdcount.c

$(BUILD)/fuzz_http_parser: fuzz/fuzz_http_parser.c $(FUZZ_PARSER_SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) -O1 -g -DFUZZ_STANDALONE \
	    -I$(SRC) $< $(FUZZ_PARSER_SRCS) -o $@ $(LDFLAGS)

$(BUILD)/fuzz_path: fuzz/fuzz_path.c $(FUZZ_PATH_SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) -O1 -g -DFUZZ_STANDALONE \
	    -I$(SRC) $< $(FUZZ_PATH_SRCS) -o $@ $(LDFLAGS)

$(BUILD)/fuzz_escape: fuzz/fuzz_escape.c $(FUZZ_ESCAPE_SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(DEFS) $(WARN) -O1 -g -DFUZZ_STANDALONE \
	    -I$(SRC) $< $(FUZZ_ESCAPE_SRCS) -o $@ $(LDFLAGS)

fuzz: $(BUILD)/fuzz_http_parser $(BUILD)/fuzz_path $(BUILD)/fuzz_escape
	./$(BUILD)/fuzz_http_parser
	./$(BUILD)/fuzz_path
	./$(BUILD)/fuzz_escape

# libFuzzer campaigns. These need clang; the CI runs the standalone drivers
# above with plain gcc, which cover the same invariants with a fixed corpus.
#
#   make fuzz-long           30 minutes per target (a smoke campaign)
#   make fuzz-long HOURS=24  the 24-hour campaign the roadmap asks for
#
# A crash writes a reproducer to fuzz/artifacts/ and the target's exit status
# becomes non-zero, so the campaign cannot pass silently over a finding.
FUZZ_CC ?= clang
FUZZ_FLAGS := -std=c11 -D_GNU_SOURCE -O1 -g -fsanitize=fuzzer,address,undefined \
              -fno-sanitize-recover=undefined -fno-omit-frame-pointer -I$(SRC)
FUZZ_CORPUS := $(CURDIR)/fuzz/corpus
FUZZ_ARTIFACTS := $(CURDIR)/fuzz/artifacts
HOURS ?= 0

# Soak test parameters. The roadmap's one-hour run is `make soak`.
SOAK_SECONDS ?= 300
SOAK_CLIENTS ?= 200
SOAK_ACTIVE ?= 4
SOAK_RATE ?= 50
FUZZ_SECONDS := $(shell if [ "$(HOURS)" != "0" ]; then echo $$(( $(HOURS) * 3600 )); else echo 1800; fi)

$(BUILD)/fuzz_parser_libfuzzer: fuzz/fuzz_http_parser.c $(FUZZ_PARSER_SRCS) $(HDRS)
	@mkdir -p $(dir $@) $(FUZZ_ARTIFACTS)/parser $(FUZZ_CORPUS)/parser
	$(FUZZ_CC) $(FUZZ_FLAGS) $< $(FUZZ_PARSER_SRCS) -o $@

$(BUILD)/fuzz_path_libfuzzer: fuzz/fuzz_path.c $(FUZZ_PATH_SRCS) $(HDRS)
	@mkdir -p $(dir $@) $(FUZZ_ARTIFACTS)/path $(FUZZ_CORPUS)/path
	$(FUZZ_CC) $(FUZZ_FLAGS) $< $(FUZZ_PATH_SRCS) -o $@

$(BUILD)/fuzz_escape_libfuzzer: fuzz/fuzz_escape.c $(FUZZ_ESCAPE_SRCS) $(HDRS)
	@mkdir -p $(dir $@) $(FUZZ_ARTIFACTS)/escape $(FUZZ_CORPUS)/escape
	$(FUZZ_CC) $(FUZZ_FLAGS) $< $(FUZZ_ESCAPE_SRCS) -o $@

fuzz-long: $(BUILD)/fuzz_parser_libfuzzer $(BUILD)/fuzz_path_libfuzzer $(BUILD)/fuzz_escape_libfuzzer
	@echo "=== parser: $(FUZZ_SECONDS)s ==="
	./$(BUILD)/fuzz_parser_libfuzzer -max_total_time=$(FUZZ_SECONDS) \
	    -artifact_prefix=$(FUZZ_ARTIFACTS)/parser/ $(FUZZ_CORPUS)/parser
	@echo "=== path: $(FUZZ_SECONDS)s ==="
	./$(BUILD)/fuzz_path_libfuzzer -max_total_time=$(FUZZ_SECONDS) \
	    -artifact_prefix=$(FUZZ_ARTIFACTS)/path/ $(FUZZ_CORPUS)/path
	@echo "=== escape: $(FUZZ_SECONDS)s ==="
	./$(BUILD)/fuzz_escape_libfuzzer -max_total_time=$(FUZZ_SECONDS) \
	    -artifact_prefix=$(FUZZ_ARTIFACTS)/escape/ $(FUZZ_CORPUS)/escape
	@echo "fuzz-long: $(FUZZ_SECONDS)s per target, no crash"

fuzz-replay:
	@echo "=== replaying every saved corpus and artifact ==="
	./$(BUILD)/fuzz_parser_libfuzzer -runs=0 $(FUZZ_CORPUS)/parser $(FUZZ_ARTIFACTS)/parser
	./$(BUILD)/fuzz_path_libfuzzer -runs=0 $(FUZZ_CORPUS)/path $(FUZZ_ARTIFACTS)/path
	./$(BUILD)/fuzz_escape_libfuzzer -runs=0 $(FUZZ_CORPUS)/escape $(FUZZ_ARTIFACTS)/escape
	@echo "fuzz-replay: every input still passes"

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

# --- Generated header dependencies ----------------------------------------
-include $(DEPS)
