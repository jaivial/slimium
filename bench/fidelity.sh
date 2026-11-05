#!/usr/bin/env bash
# Screenshot fidelity: the same fixture pages captured with a reference config
# (default: stock) and with CONFIG at the same viewport/DPR, compared pixel by
# pixel (pngdiff.py; a pixel differs when a channel moves by more than 24).
#   fidelity.sh CONFIG [REFCONFIG]     appends one JSON line to results/fidelity.jsonl
# Fixtures: text + web font, images (PNG/SVG/background), canvas 2D (viewport and
# element clip), SVG, CSS gradients/transforms/filters, a WebGL triangle, a long
# page captured full-page, and en.wikipedia.org/wiki/Linux (live; looser).
# A deterministic fixture over 1% different fails the config.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BENCH_LIB=1 . "$HERE/bench.sh"
cfg=$1 ref=${2:-stock}
mkdir -p "$HERE/www/gen"
[ -s "$HERE/www/gen/pattern.png" ] || python3 "$HERE/pngdiff.py" --pattern "$HERE/www/gen/pattern.png"
[ -s "$HERE/www/gen/webfont.otf" ] || cp /usr/share/fonts/urw-base35/URWBookman-LightItalic.otf "$HERE/www/gen/webfont.otf"
start_httpd
FX="text images canvas canvas-clip svg css webgl long-full wikipedia"
fx_url() { case $1 in canvas-clip) echo "$H/fid/canvas.html";; long-full) echo "$H/fid/long.html";;
  wikipedia) echo "https://en.wikipedia.org/wiki/Linux";; *) echo "$H/fid/$1.html";; esac; }
fx_shot() { case $1 in canvas-clip) echo '{"format":"png","clip":{"x":20,"y":20,"width":600,"height":400,"scale":1}}';;
  long-full) echo '{"format":"png","captureBeyondViewport":true,"clip":{"x":0,"y":0,"width":1000,"height":3624,"scale":1}}';;
  *) echo '{"format":"png"}';; esac; }

capture() {   # $1 config, $2 out dir
  mkdir -p "$2"; launch "$1" || return 1
  local B fx out; B=$(browser_ws)
  for fx in $FX; do
    out=$("$BIN/slim-cdp" -m 0 -t 40 "$B" 'Target.createTarget {"url":"about:blank"}' \
      'Target.attachToTarget {"targetId":"$targetId","flatten":true}' \
      'S:Emulation.setDeviceMetricsOverride {"width":1000,"height":800,"deviceScaleFactor":1,"mobile":false}' \
      "S:Page.navigate {\"url\":\"$(fx_url $fx)\"}" 'sleep 4000' \
      "S:Page.captureScreenshot $(fx_shot $fx)" \
      'S:Runtime.evaluate {"expression":"document.title","returnByValue":true}' \
      'Target.closeTarget {"targetId":"$targetId"}' 2>&1)
    echo "$out" | jq -r 'select(.result.data) | .result.data' 2>/dev/null | base64 -d > "$2/$fx.png" 2>/dev/null
    echo "$out" | jq -r 'select(.result.result.value) | .result.result.value' 2>/dev/null > "$2/$fx.title"
  done
  stop
}
D=$RES/fid/$(basename "$cfg")
capture "$ref" "$D/ref" || exit 1
capture "$cfg" "$D/cfg" || exit 1
res="{}" fail=""
for fx in $FX; do
  if [ -s "$D/ref/$fx.png" ] && [ -s "$D/cfg/$fx.png" ]; then
    d=$(python3 "$HERE/pngdiff.py" "$D/ref/$fx.png" "$D/cfg/$fx.png")
  else d='{"diff_pct":100,"missing":true}'; fi
  d=$(echo "$d" | jq -c --arg t "$(cat "$D/cfg/$fx.title" 2>/dev/null)" --arg rt "$(cat "$D/ref/$fx.title" 2>/dev/null)" '. + {title:$t, ref_title:$rt}')
  res=$(echo "$res" | jq -c --arg k "$fx" --argjson v "$d" '. + {($k): $v}')
  p=$(echo "$d" | jq -r .diff_pct)
  limit=1; [ $fx = wikipedia ] && limit=5
  awk -v p="$p" -v l=$limit 'BEGIN{exit !(p > l)}' && fail+="$fx($p%) "
done
jq -cn --arg c "$(basename "$cfg")" --arg r "$(basename "$ref")" --arg f "${fail:-pass}" --argjson p "$res" \
  '{config:$c, ref:$r, date:(now|todate), fidelity:($f|rtrimstr(" ")), pages:($p|map_values(.diff_pct)), detail:$p}' | tee -a "$RES/fidelity.jsonl" | jq -c '{config,fidelity,pages}'
