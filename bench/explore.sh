#!/usr/bin/env bash
# Measure every candidate of candidates.txt (or the names given) as a delta on
# top of a base config:  explore.sh [-b BASE] [-w A|B] [-r REPS] [NAME...]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BASE=stock W=A REPS=2
while getopts b:w:r: o; do case $o in b) BASE=$OPTARG;; w) W=$OPTARG;; r) REPS=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND - 1))
mkdir -p "$HERE/configs/explore"
while IFS='|' read -r name flags; do
  name=$(echo "$name" | xargs); case "$name" in ''|'#'*) continue;; esac
  if [ $# -gt 0 ]; then hit=0; for n in "$@"; do [ "$n" = "$name" ] && hit=1; done; [ $hit = 1 ] || continue; fi
  f=$HERE/configs/explore/$BASE+$name
  { grep -v '^#' "$HERE/configs/$BASE"; echo "$flags" | sed 's/^ *//; s/ *$//; s/ *;; */\n/g'; } > "$f"
  TAG=explore "$HERE/bench.sh" run "$f" "$W" "$REPS"
done < "$HERE/candidates.txt"
