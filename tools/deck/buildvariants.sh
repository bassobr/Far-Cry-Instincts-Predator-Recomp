#!/bin/bash
# buildvariants.sh: first-start game code build per compiler variant on the Steam Deck, then the main thread in the
# game with that DLL (docs/phase1/deck-erststart-rumble.md). Per variant a fresh portable folder (release files of
# APP_DIR plus its shaders/, so only the game code is built; a fresh work folder, no LTO cache), first start into
# 21_dunea, then per level one warm-up and RUNS measured runs at 90 frames/s with the game's own pacing (CPU bound).
# Start: ~/predator-test/tools/start.sh bv buildvariants.sh APP_DIR=$HOME/predator-test/app-v012p
# Variables: VARIANTS "name|VAR=value ...;..." (default below), LEVELS, RUNS (default 2), KEEP=1 keeps the folders.
# Results: runs/B_<variant>_build (first start), runs/B_<variant>_<level>_<n> (perf); status in runs/bv.txt.
T=${PREDATOR_TEST:-$HOME/predator-test}
X=$T/tools/run.sh
for kv in "$@"; do export "$kv"; done
APP=${APP_DIR:-$T/app-v012p}
VARIANTS=${VARIANTS:-"base|;cap0|FCP_GAME_CODE_CAPTURE_USES=0;lto0|FCP_GAME_CODE_LTO=0;cap10k|FCP_GAME_CODE_CAPTURE_USES=10000;old|FCP_GAME_CODE_CAPTURE_USES=0 FCP_GAME_CODE_LTO=0"}
LEVELS=${LEVELS:-21_dunea 06_cliffs}
RUNS=${RUNS:-2}
log() { echo "$(date +%T) $*" >> "$T/runs/bv.txt"; }
IFS=';' read -ra items <<< "$VARIANTS"
for it in "${items[@]}"; do
  v=${it%%|*}; vars=${it#*|}
  F=$T/bv-$v
  rm -rf "$F"; mkdir -p "$F"
  (cd "$APP" && for f in *; do case "$f" in game_code|cache|logs|predator.log*|predator.gen*|pipelines.*.bin|vk_pipelines.bin) ;; *) cp -a "$f" "$F/";; esac; done)
  log "$v: build ($vars)"
  # shellcheck disable=SC2086
  "$X" "B_${v}_build" 20 APP_DIR=$F FCP_USER_ROOT_UNIX=$F FCP_NO_DIALOGS=1 FCP_START_LEVEL=21_dunea FCP_FPS_STATS=5 \
    FCP_EXIT_AFTER=30 WAIT_LINE='framestat: .*level 21' MAX_WAIT=1800 $vars
  log "$v: $(grep -a 'first-start build in' "$T/runs/B_${v}_build/predator.log" | sed 's/.*game code: //')"
  [ -f "$F/game_code/predator_code.dll" ] || { log "$v: no DLL, skipped"; continue; }
  for lv in $LEVELS; do
    for n in w $(seq 1 "$RUNS"); do
      "$X" "B_${v}_${lv}_$n" 130 APP_DIR=$F FCP_USER_ROOT_UNIX=$F FCP_NO_DIALOGS=1 FCP_START_LEVEL=$lv FCP_FPS=90 \
        FCP_FPS_PACING=game FCP_CPU_STATS=5 FCP_FPS_STATS=5 FCP_INPUT_SCRIPT_CLOCK=level FCP_INPUT_SCRIPT=10-50:lup \
        FCP_EXIT_AFTER=60
      log "$v: $lv $n done"
      sleep 3
    done
  done
  [ "${KEEP:-0}" = 1 ] || rm -rf "$F"
done
log "variants done"
