#!/usr/bin/env bash
# Runtime memory governor study: what the DevTools protocol can reclaim from a
# running browser, measured step by step on the same tabs.
#   governor.sh CONFIG [A|B]      appends one JSON line to results/governor.jsonl
# Steps (each followed by 4 s and a 3-sample median PSS):
#   base       after the workload settled
#   gc         HeapProfiler.collectGarbage in every page
#   pressure   Memory.simulatePressureNotification critical (browser + every page)
#   freeze     Page.setWebLifecycleState frozen for every page but the first
#   minimize   Browser.setWindowBounds minimized for the same background pages
#              (the renderer sees them hidden: drops tiles, backgrounds V8)
#   min+press  pressure + gc again while minimized/frozen
#   later      30 s later (does it grow back?)
#   resume     windows normal + lifecycle active; every tab must evaluate 1+1
# (Memory.forciblyPurgeJavaScriptMemory is deliberately not used: it destroys
#  the page's V8 contexts and Runtime.evaluate fails until a reload.)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BENCH_LIB=1 . "$HERE/bench.sh"
cfg=$1 w=${2:-A}
launch "$cfg" || exit 1
open_workload "$w"
declare -A M
step() { M[$1]=$(sample); echo "$1: $(echo "${M[$1]}" | jq -r '"iso \(.iso_kb/1024|floor) MB anon \(.anon_kb/1024|floor) pss \(.pss_kb/1024|floor) procs \(.procs)"')" >&2; }
each_page() { local ws; for ws in $(page_ws); do "$BIN/slim-cdp" -t 10 "$ws" "$@" >/dev/null 2>&1; done; }
bg_targets() { curl -s "http://127.0.0.1:$PORT/json/list" | jq -r '[.[] | select(.type=="page")] | .[1:][] | .id'; }
pressure() {
  "$BIN/slim-cdp" -t 10 "$(browser_ws)" 'Memory.simulatePressureNotification {"level":"critical"}' >/dev/null 2>&1
  each_page 'Memory.simulatePressureNotification {"level":"critical"}'
}
step base
each_page 'HeapProfiler.collectGarbage'; sleep 4; step gc
pressure; sleep 4; step pressure
for ws in $(page_ws | tail -n +2); do "$BIN/slim-cdp" -t 10 "$ws" 'Page.setWebLifecycleState {"state":"frozen"}' >/dev/null 2>&1; done
sleep 4; step freeze
B=$(browser_ws)
for id in $(bg_targets); do
  "$BIN/slim-cdp" -t 10 "$B" "Browser.getWindowForTarget {\"targetId\":\"$id\"}" \
     'Browser.setWindowBounds {"windowId":$windowId,"bounds":{"windowState":"minimized"}}' >/dev/null 2>&1 \
     || echo "minimize failed for $id" >&2
done 2>/dev/null
sleep 4; step minimize
pressure; each_page 'HeapProfiler.collectGarbage'; sleep 4; step min+press
sleep 30; step later
for id in $(bg_targets); do
  wid=$("$BIN/slim-cdp" -t 10 "$B" "Browser.getWindowForTarget {\"targetId\":\"$id\"}" | jq -r .result.windowId)
  "$BIN/slim-cdp" -t 10 "$B" "Browser.setWindowBounds {\"windowId\":$wid,\"bounds\":{\"windowState\":\"normal\"}}" >/dev/null 2>&1
done
each_page 'Page.setWebLifecycleState {"state":"active"}'
ok=0 n=0; for ws in $(page_ws); do n=$((n+1)); [ "$(eval_ws "$ws" '1+1')" = 2 ] && ok=$((ok+1)); done
stop
jq -cn --arg c "$(basename "$cfg")" --arg w "$w" --argjson ok $ok --argjson n $n \
  --argjson base "${M[base]}" --argjson gc "${M[gc]}" --argjson pr "${M[pressure]}" \
  --argjson fr "${M[freeze]}" --argjson mi "${M[minimize]}" --argjson mp "${M[min+press]}" --argjson la "${M[later]}" \
  '{config:$c, workload:$w, date:(now|todate), resume_ok:"\($ok)/\($n)",
    steps: ([["base",$base],["gc",$gc],["pressure",$pr],["freeze",$fr],["minimize",$mi],["min+press",$mp],["later",$la]]
            | map({step:.[0], iso_kb:.[1].iso_kb, anon_kb:.[1].anon_kb, pss_kb:.[1].pss_kb, uss_kb:.[1].uss_kb, procs:.[1].procs}))}' | tee -a "$RES/governor.jsonl"
