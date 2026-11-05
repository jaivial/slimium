# bench/Makefile: memory-track tools (C11, same flags as the top-level build).
CC      ?= gcc
CFLAGS  ?= -std=c11 -Os -Wall -Wextra -Werror -Wshadow -Wformat=2 -Wno-format-nonliteral -pedantic
LDFLAGS ?= -s
OUT     := ../build/bench
SRC     := ../src

TOOLS := $(OUT)/slim-memstat $(OUT)/slim-cdp $(OUT)/slim-httpd $(OUT)/slim-merge $(OUT)/test_profile

all: $(TOOLS)

$(OUT):
	mkdir -p $@

$(OUT)/slim-memstat: slim-memstat.c $(SRC)/mem.c $(SRC)/mem.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ slim-memstat.c $(SRC)/mem.c $(LDFLAGS)

$(OUT)/slim-cdp: slim-cdp.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ slim-cdp.c $(LDFLAGS)

$(OUT)/slim-httpd: slim-httpd.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ slim-httpd.c $(LDFLAGS)

$(OUT)/slim-merge: slim-merge.c $(SRC)/profile.c $(SRC)/profile.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ slim-merge.c $(SRC)/profile.c $(LDFLAGS)

$(OUT)/test_profile: test_profile.c $(SRC)/profile.c $(SRC)/profile.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_profile.c $(SRC)/profile.c

test: $(OUT)/test_profile $(OUT)/slim-memstat
	$(OUT)/test_profile
	$(OUT)/slim-memstat $$$$ >/dev/null

bench: all
	./bench.sh

clean:
	rm -rf $(OUT)

.PHONY: all test bench clean
