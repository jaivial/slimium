#!/usr/bin/env bash
# tests/cu/variants.sh -- which engine flags break cu, and what they save.
# For each variant: tests/cu/run.sh (pass/fail + failing checks) and one
# tests/cu/ram.sh run (PSS through cu). Variants use the stand-in's knobs
# (SLIM_OVERRIDE / SLIM_EXTRA / SLIM_ENGINE). Results: tests/cu/results/variants-*.txt
set -u
cd "$(dirname "$0")" || exit 2
HS=${HS:-$(ls -d $HOME/.cache/ms-playwright/chromium_headless_shell-*/chrome-headless-shell-linux64/chrome-headless-shell 2>/dev/null | tail -1)}
OUT=${OUT:-results/variants-$(date +%Y%m%d-%H%M%S).txt}
mkdir -p results
L=--renderer-process-limit
COMBO="--in-process-gpu --enable-features=NetworkServiceInProcess2 --no-zygote --no-sandbox --js-flags=--lite-mode"
variants=(
  "base|"
  "limit1|SLIM_OVERRIDE=$L"
  "limit1+in-process-gpu|SLIM_OVERRIDE=$L SLIM_EXTRA=--in-process-gpu"
  "limit1+NetworkServiceInProcess2|SLIM_OVERRIDE=$L SLIM_EXTRA=--enable-features=NetworkServiceInProcess2"
  "limit1+no-zygote+no-sandbox|SLIM_OVERRIDE=$L SLIM_EXTRA='--no-zygote --no-sandbox'"
  "limit1+no-sandbox|SLIM_OVERRIDE=$L SLIM_EXTRA=--no-sandbox"
  "limit1+lite-mode|SLIM_OVERRIDE=$L SLIM_EXTRA=--js-flags=--lite-mode"
  "limit1+jitless|SLIM_OVERRIDE=$L SLIM_EXTRA=--js-flags=--jitless"
  "limit1+single-process|SLIM_OVERRIDE=$L SLIM_EXTRA=--single-process"
  "limit1+combo|SLIM_OVERRIDE=$L SLIM_EXTRA='$COMBO'"
  "limit1+NSIP2+noWebUIOmnibox|SLIM_OVERRIDE=$L SLIM_EXTRA='--enable-features=NetworkServiceInProcess2 --disable-features=WebUIOmniboxPopup,WebUIOmniboxAimPopup --disable-component-extensions-with-background-pages'"
  "limit1+NSIP2+noWebUIOmnibox+in-process-gpu|SLIM_OVERRIDE=$L SLIM_EXTRA='--enable-features=NetworkServiceInProcess2 --disable-features=WebUIOmniboxPopup,WebUIOmniboxAimPopup --disable-component-extensions-with-background-pages --in-process-gpu'"
  "headless-shell(stock,direct)|DIRECT=$HS"
  "headless-shell+limit1+combo|SLIM_ENGINE=$HS SLIM_OVERRIDE=$L SLIM_EXTRA='$COMBO'"
)
[ -n "${ONLY:-}" ] && { keep=(); for v in "${variants[@]}"; do [[ "${v%%|*}" == $ONLY ]] && keep+=("$v"); done; variants=("${keep[@]}"); }
echo "# variant | suite | pss_MB procs | failing checks   ($(date -Is))" | tee "$OUT"
for v in "${variants[@]}"; do
  name=${v%%|*} envs=${v#*|}
  ( unset SLIM_OVERRIDE SLIM_EXTRA SLIM_ENGINE DIRECT
    eval "export $envs" 2>/dev/null
    if [ -n "${DIRECT:-}" ]; then sb="CU_SLIM_TEST_BROWSER=$DIRECT"; rb="RAM_BROWSER=$DIRECT CONFIGS=custom"; unset DIRECT
    else sb=""; rb="CONFIGS=slim-env"; fi
    suite=$(env $sb ./run.sh 2>&1)
    sum=$(printf '%s\n' "$suite" | grep '^# [0-9]* passed' | sed 's/^# //')
    ram=$(env $rb RUNS=1 SETTLE=3 OUT=/tmp/variants-ram.txt ./ram.sh 2>/dev/null | grep -E '^(slim-env|custom) 1 ' | awk '{printf "%.0f MB %d procs %s snaps", $3/1024, $5, $6}')
    echo "$name | $sum | ${ram:-ram failed}" | tee -a "$OUT"
    printf '%s\n' "$suite" | grep '^FAIL' | cut -c1-160 | sed 's/^/    /' | tee -a "$OUT"
  )
done
