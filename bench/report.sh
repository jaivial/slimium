#!/usr/bin/env bash
# Summarise results/raw.jsonl as a markdown table: per (config, workload) the
# median iso/anon/PSS (MB), procs, runs, and iso as a fraction of the baseline.
# Runs where a tab never finished loading (empty title) are left out.
#   report.sh [TAG] [BASELINE=stock]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TAG=${1:-} BASE=${2:-stock}
jq -rs --arg tag "$TAG" --arg base "$BASE" '
  def med: sort | if length == 0 then null else .[(length/2|floor)] end;
  def mb: if . == null then "-" else (./1024|floor|tostring) end;
  [ .[] | select(.iso_kb and ($tag == "" or .tag == $tag) and ((.titles // []) | all(. != ""))) ]
  | group_by([.workload, .config])
  | map({w: .[0].workload, c: .[0].config, n: length, iso: (map(.iso_kb)|med), anon: (map(.anon_kb)|med),
         pss: (map(.pss_kb)|med), procs: (map(.procs)|med)})
  | . as $rows
  | ("| workload | config | runs | iso MB | anon MB | PSS MB | procs | iso vs \($base) |",
     "|---|---|---|---|---|---|---|---|",
     ($rows[] | . as $r | ($rows | map(select(.w == $r.w and .c == $base)) | .[0].iso) as $b
      | "| \(.w) | \(.c) | \(.n) | \(.iso|mb) | \(.anon|mb) | \(.pss|mb) | \(.procs) | \(if $b then ((.iso / $b * 100 | floor | tostring) + "%") else "-" end) |"))
' "$HERE/results/raw.jsonl"
