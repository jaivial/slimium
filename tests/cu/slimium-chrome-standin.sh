#!/usr/bin/env bash
# slimium-chrome stand-in: exec the system Chromium with slimium's low-memory
# profile (src/profile.c) merged into whatever argv the caller (cu) passes.
# Used by tests/cu until the real C wrapper (core track) is installed.
#
# Merge rules (same contract as slim_profile_merge_argv in src/profile.h):
#  - caller flags are kept in order;
#  - a profile flag whose switch name the caller already set is dropped
#    (the caller wins), except the list-valued switches below;
#  - every --enable-features / --disable-features value (caller's and the
#    profile's) is unioned into ONE switch each: Chromium only honours the
#    last occurrence, so appending a second one would silently drop cu's;
#  - --js-flags values are concatenated.
# SLIM_PROFILE=off|balanced|max (default balanced). SLIM_ENGINE overrides the
# engine executable. Default: the Chromium ELF itself, like the real wrapper,
# not Fedora's /usr/bin/chromium-browser script (which adds
# --enable-extensions/--enable-sync/--enable-plugins and keeps two `cat`
# processes alive); SLIM_ENGINE_DIRECT=0 goes through the script.
set -u
if [ -n "${SLIM_ENGINE:-}" ]; then engine=$SLIM_ENGINE
elif [ "${SLIM_ENGINE_DIRECT:-1}" = 0 ]; then engine=/usr/bin/chromium-browser
else engine=/usr/lib64/chromium-browser/chromium-browser
fi
level=${SLIM_PROFILE:-balanced}

common=(
  --renderer-process-limit=1
  --process-per-site
  --disable-site-isolation-trials
  --disable-features=IsolateOrigins,site-per-process,Translate,OptimizationHints,MediaRouter,BackForwardCache,InterestFeedContentSuggestions,CalculateNativeWinOcclusion,AutofillServerCommunication
  --enable-low-end-device-mode
  --disable-gpu
  --disable-extensions
  --disable-component-extensions-with-background-pages
  --disable-background-networking
  --disable-default-apps
  --disable-sync
  --no-first-run
  --disable-breakpad
)
case $level in
  off|0) profile=() ;;
  balanced|1) profile=("${common[@]}" "--js-flags=--max-lazy") ;;
  max|2) profile=("${common[@]}" --enable-features=NetworkServiceInProcess2 --in-process-gpu "--js-flags=--max-lazy --optimize-for-size") ;;
  *) echo "slimium-chrome: unknown SLIM_PROFILE '$level' (off|balanced|max)" >&2; exit 2 ;;
esac

# Version/help probes go straight to the engine (cu runs --version for its UA).
for a in "$@"; do
  case $a in --version|-version|--product-version) exec "${SLIM_ENGINE:-/usr/bin/chromium-browser}" "$a" ;; esac
done

out=() enable="" disable="" js="" pos=()
declare -A seen=()
add_list() { # $1=var name, $2=comma list: union, keep first-seen order
  local -n acc=$1; local item
  IFS=, read -ra items <<<"$2"
  for item in "${items[@]}"; do
    [ -z "$item" ] && continue
    case ",$acc," in *",$item,"*) ;; *) acc="${acc:+$acc,}$item" ;; esac
  done
}
take() { # $1=arg, $2=from caller (1) or profile (0)
  local a=$1 name
  case $a in
    --enable-features=*) add_list enable "${a#*=}"; return ;;
    --disable-features=*) add_list disable "${a#*=}"; return ;;
    --js-flags=*) js="${js:+$js }${a#*=}"; return ;;
  esac
  if [[ $a == --* ]]; then
    name=${a%%=*}
    if [ "$2" = 0 ] && [ -n "${seen[$name]:-}" ]; then return; fi
    seen[$name]=1
    out+=("$a")
  else
    pos+=("$a")   # positional (start URL): after all switches
  fi
}
for a in "$@"; do take "$a" 1; done
for a in "${profile[@]}"; do take "$a" 0; done
[ -n "$enable" ] && out+=("--enable-features=$enable")
[ -n "$disable" ] && out+=("--disable-features=$disable")
[ -n "$js" ] && out+=("--js-flags=$js")
[ -n "${SLIM_STANDIN_LOG:-}" ] && printf '%s\n' "$engine ${out[*]} ${pos[*]}" >>"$SLIM_STANDIN_LOG"
exec "$engine" "${out[@]}" "${pos[@]}"
