# Terminal Tedium - build system
#
#   make            build libtedium and the command line tools
#   make test       build and run the test suite (host, no hardware needed)
#   make test SAN=1 same, under AddressSanitizer and UBSan
#   make bindings   build every engine binding whose SDK is installed
#   make install    install library, tools and bindings
#   make clean
#
# Everything except the Linux HAL and the JACK binding builds on macOS, so the
# core can be developed and tested without the module attached.

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

CC      ?= cc
OPT     ?= -O2
PREFIX  ?= /usr/local

BUILD   := build

WARN := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
        -Wmissing-prototypes -Wpointer-arith -Wcast-qual -Wno-unused-parameter

CFLAGS  := -std=c11 $(WARN) $(OPT) -fPIC -Iinclude -Isrc
LDLIBS  := -lm

ifeq ($(UNAME_S),Linux)
  CFLAGS += -D_GNU_SOURCE
  LDLIBS += -lpthread
  SOEXT  := so
  SOFLAG := -shared
else
  SOEXT  := dylib
  SOFLAG := -dynamiclib
endif

ifdef SAN
  CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
  LDFLAGS += -fsanitize=address,undefined
endif

# The rings are lock-free, so a data race is the failure mode that ordinary
# testing will not catch. TSan and ASan cannot be combined; run both.
ifdef TSAN
  CFLAGS += -fsanitize=thread -fno-omit-frame-pointer -g -O1
  LDFLAGS += -fsanitize=thread
endif

ifdef DEBUG
  CFLAGS += -g -O0 -DTT_DEBUG=1
endif

# ------------------------------------------------------------------ #
# core library                                                        #
# ------------------------------------------------------------------ #

LIB_SRC := src/tt_ring.c src/tt_board.c src/tt_cal.c src/tt_time.c \
           src/tt_core.c src/hal_sim.c

ifeq ($(UNAME_S),Linux)
  LIB_SRC += src/hal_linux.c
endif

LIB_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SRC))

STATIC  := $(BUILD)/libtedium.a
SHARED  := $(BUILD)/libtedium.$(SOEXT)

# ------------------------------------------------------------------ #
# tools                                                               #
# ------------------------------------------------------------------ #

TOOL_SRC  := tools/tedium-monitor.c tools/tedium-cal.c tools/tedium-bench.c
TOOL_BIN  := $(patsubst tools/%.c,$(BUILD)/%,$(TOOL_SRC))

# ------------------------------------------------------------------ #
# tests                                                               #
# ------------------------------------------------------------------ #

TEST_SRC := $(wildcard tests/*.c)
TEST_OBJ := $(patsubst tests/%.c,$(BUILD)/tests/%.o,$(TEST_SRC))
TEST_BIN := $(BUILD)/tt_tests

# ------------------------------------------------------------------ #

.PHONY: all lib tools test bindings clean install dirs help

all: lib tools

help:
	@echo "targets: all lib tools test bindings install clean"
	@echo "platform: $(UNAME_S) $(UNAME_M), hal: $(if $(filter Linux,$(UNAME_S)),linux+sim,sim only)"

dirs:
	@mkdir -p $(BUILD)/tests

lib: dirs $(STATIC) $(SHARED)

$(BUILD)/%.o: src/%.c | dirs
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tests/%.o: tests/%.c | dirs
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests -c $< -o $@

$(STATIC): $(LIB_OBJ)
	@rm -f $@
	ar rcs $@ $^

$(SHARED): $(LIB_OBJ)
	$(CC) $(SOFLAG) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tools: dirs $(TOOL_BIN)

$(BUILD)/%: tools/%.c $(STATIC) | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(STATIC) $(LDLIBS)

$(TEST_BIN): $(TEST_OBJ) $(STATIC)
	$(CC) $(LDFLAGS) -o $@ $(TEST_OBJ) $(STATIC) $(LDLIBS)

test: dirs $(TEST_BIN)
	@$(TEST_BIN) $(FILTER)

# Load each binding in its real host. Requires `make bindings' first; engines
# that are not installed are skipped rather than failed.
.PHONY: test-integration
test-integration:
	@sh tests/integration/run.sh

bindings: lib
	@$(MAKE) -C bindings --no-print-directory

install: all
	install -d $(PREFIX)/lib $(PREFIX)/bin $(PREFIX)/include/tedium
	install -m 644 $(STATIC) $(SHARED) $(PREFIX)/lib/
	install -m 644 include/tedium/tedium.h $(PREFIX)/include/tedium/
	install -m 755 $(TOOL_BIN) $(PREFIX)/bin/

clean:
	rm -rf $(BUILD)
	@$(MAKE) -C bindings clean --no-print-directory 2>/dev/null || true

# Parse the Linux HAL on a non-Linux machine, against stub kernel uapi
# headers. This catches syntax and type errors in our own code; it does NOT
# validate the real struct layouts or ioctl numbers, so it is a development
# convenience, not a substitute for building on the Pi.
.PHONY: check-linux
check-linux:
	$(CC) -std=c11 $(WARN) -D__linux__ -Iinclude -Isrc \
	      -Itests/linux-uapi-stubs -fsyntax-only src/hal_linux.c
	@echo "hal_linux.c parses (stub headers; build on the Pi to verify for real)"

# Editor/LSP support. Regenerate after adding a source file.
.PHONY: compile_commands
compile_commands:
	@echo '[' > compile_commands.json
	@first=1; for f in $(LIB_SRC) $(TEST_SRC) $(TOOL_SRC); do \
	  if [ $$first -eq 0 ]; then echo ',' >> compile_commands.json; fi; first=0; \
	  printf '  {"directory": "%s", "file": "%s", "command": "%s %s -Itests -c %s"}' \
	    "$(CURDIR)" "$$f" "$(CC)" "$(CFLAGS)" "$$f" >> compile_commands.json; \
	done
	@echo '' >> compile_commands.json
	@echo ']' >> compile_commands.json
	@echo "wrote compile_commands.json"

-include $(LIB_OBJ:.o=.d)
-include $(TEST_OBJ:.o=.d)
CFLAGS += -MMD -MP
