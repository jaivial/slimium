#!/usr/bin/env bash
# Build slimium + unit tests with AddressSanitizer/UBSan (+LeakSanitizer) and run both suites.
# Uses the host compiler when it can link sanitizers; otherwise the Flatpak freedesktop SDK's gcc
# (the browser still runs on the host; sanitized slimium talks to it over loopback).
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
export SLIM_STATE_DIR=$(mktemp -d /tmp/slim-asan-state.XXXXXX)
WR=$(mktemp -d /tmp/slim-asan-bin.XXXXXX)
trap 'rm -rf "$SLIM_STATE_DIR" "$WR"' EXIT
SAN_ENV="ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1"

if printf 'int main(void){return 0;}' | ${CC:-gcc} -fsanitize=address,undefined -x c - -o "$WR/probe" 2>/dev/null; then
    make -s asan || exit 1
    printf '#!/bin/sh\nexec env %s "%s/build/asan/slimium" "$@"\n' "$SAN_ENV" "$ROOT" >"$WR/slimium"
    RUNU="env $SAN_ENV build/asan/unit"
else
    SDK=$(flatpak list --runtime --columns=application,branch 2>/dev/null | awk '$1=="org.freedesktop.Sdk"{print $1"//"$2}' | sort -V | tail -1)
    if [ -z "$SDK" ]; then echo "asan: no sanitizer runtime and no Flatpak SDK available; skipped"; exit 0; fi
    echo "asan: host gcc has no libasan; using $SDK"
    FP="flatpak run --devel --filesystem=home --filesystem=/tmp --share=network"
    for e in $SAN_ENV SLIM_STATE_DIR=$SLIM_STATE_DIR; do FP="$FP --env=$e"; done
    $FP --command=make "$SDK" -s -C "$ROOT" asan CC=gcc || exit 1
    printf '#!/bin/sh\nexec %s --command="%s/build/asan/slimium" %s "$@"\n' "$FP" "$ROOT" "$SDK" >"$WR/slimium"
    RUNU="$FP --command=$ROOT/build/asan/unit $SDK"
    export SLIM_NO_PROC=1 # host /proc (browser pid, memory) is not visible inside the sandbox
    export SLIM_HOLD=0    # no keeper: the sandboxed CLI could not validate the host one's pid
fi
chmod +x "$WR/slimium"
echo "== unit (sanitized)"
$RUNU || { echo "asan: unit tests failed"; exit 1; }
echo "== functional (sanitized slimium, host browser)"
SLIM_BIN="$WR/slimium" SLIM_METRICS=0 SLIM_NO_ENGINE_TESTS=1 SLIM_TEST_PORT=${SLIM_TEST_PORT:-9312} SLIM_HTTP_PORT=9327 tests/run.sh
