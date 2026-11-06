# slimium: one C11 binary, no dependencies. 'slimium-chrome' is a symlink (argv[0] dispatch).
ifeq ($(origin CC),default)
CC      := gcc
endif
PREFIX  ?= $(HOME)/.local
BUILD   := build
STD     := -std=c11 -D_DEFAULT_SOURCE -D_GNU_SOURCE
WARN    := -Wall -Wextra -Werror -Wshadow -Wformat=2 -Wno-format-nonliteral
CFLAGS  ?= -Os -pipe -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables
LDFLAGS ?= -Wl,--gc-sections -Wl,-O1 -Wl,--as-needed
ALLCFLAGS = $(STD) $(WARN) $(CFLAGS)

# all src/*.c; mem_stub.c only while the memory track's mem.c is absent
SRCS := $(filter-out src/mem_stub.c,$(wildcard src/*.c))
ifeq ($(wildcard src/mem.c),)
SRCS += src/mem_stub.c
endif
OBJS := $(SRCS:src/%.c=$(BUILD)/%.o)
LIBSRCS := src/util.c src/json.c src/b64.c src/ws.c
HDRS := $(wildcard src/*.h)

.PHONY: all test test-modules test-cu test-asan test-gov unit clean install uninstall asan bench size
MODULE_TESTS := tests/profiles.sh tests/interaction.sh tests/media.sh tests/protocol.sh tests/mcp.sh
all: slimium slimium-chrome

$(BUILD)/%.o: src/%.c $(HDRS) | $(BUILD)
	$(CC) $(ALLCFLAGS) -c -o $@ $<
$(BUILD):
	mkdir -p $(BUILD)

slimium: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -s -o $@ $(OBJS)
slimium-chrome: slimium
	ln -sf slimium slimium-chrome

$(BUILD)/unit: tests/unit.c $(LIBSRCS) $(HDRS) | $(BUILD)
	$(CC) $(STD) $(WARN) -O1 -g -Isrc -o $@ tests/unit.c $(LIBSRCS)

unit: $(BUILD)/unit
	$(BUILD)/unit

# AddressSanitizer + UBSan builds (CLI + unit). Needs a compiler with libasan/libubsan;
# tests/asan.sh falls back to the Flatpak SDK's gcc when the host has none.
SANFLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
asan: $(SRCS) $(HDRS) tests/unit.c
	mkdir -p $(BUILD)/asan
	$(CC) $(STD) $(WARN) $(SANFLAGS) -o $(BUILD)/asan/slimium $(SRCS)
	ln -sf slimium $(BUILD)/asan/slimium-chrome
	$(CC) $(STD) $(WARN) $(SANFLAGS) -Isrc -o $(BUILD)/asan/unit tests/unit.c $(LIBSRCS)

test: all unit
	tests/run.sh
	tests/gov.sh
	@$(MAKE) -s test-modules

# Optional module suites are present after their tracks are merged.
test-modules: all
	@set -e; for t in $(MODULE_TESTS); do if [ -f "$$t" ]; then echo "== $$t"; bash "$$t"; fi; done

# End-to-end second-cu checks (isolated from the user's daemon).
test-cu: all
	bash tests/cu/run.sh
	@if [ -f tests/cu/mcp-integration.sh ]; then bash tests/cu/mcp-integration.sh; fi

# memory governor: scope, reclaim limited to the browser's own cgroups, hibernation, wrapper (CDP 9440-9442)
test-gov: all
	tests/gov.sh

# sanitizer run of unit + functional suites (see tests/asan.sh)
test-asan:
	tests/asan.sh

bench: all
	@if [ -x bench/run.sh ]; then bench/run.sh; else echo "bench/run.sh not present (memory track)"; fi

size: slimium
	@ls -l slimium | awk '{print "slimium stripped: " $$5 " bytes"}'

install: all
	mkdir -p $(PREFIX)/bin
	install -m 755 slimium $(PREFIX)/bin/slimium
	ln -sf slimium $(PREFIX)/bin/slimium-chrome

uninstall:
	rm -f $(PREFIX)/bin/slimium $(PREFIX)/bin/slimium-chrome

clean:
	rm -rf $(BUILD) slimium slimium-chrome
