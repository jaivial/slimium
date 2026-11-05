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

.PHONY: all test unit clean install uninstall asan bench size
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
	$(CC) $(STD) $(WARN) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc -o $@ tests/unit.c $(LIBSRCS)

unit: $(BUILD)/unit
	$(BUILD)/unit

# AddressSanitizer + UBSan build of the full CLI (used by tests/run.sh ASAN=1)
asan: $(SRCS) $(HDRS) | $(BUILD)
	mkdir -p $(BUILD)/asan
	$(CC) $(STD) $(WARN) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -o $(BUILD)/asan/slimium $(SRCS)
	ln -sf slimium $(BUILD)/asan/slimium-chrome

test: all unit
	tests/run.sh

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
