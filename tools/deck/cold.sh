#!/bin/bash
# cold.sh: cold first visits on the Steam Deck (docs/phase1/deck-v0.1.2.md): every run in a fresh portable user
# folder (copy of APP_DIR with only game_code/ and shaders/ kept: no pipeline list, no Vulkan pipeline cache, no shader
# disk cache) and with an empty MESA_SHADER_CACHE_DIR, so the driver compiles every pipeline as on a player's first
# visit. Start through start.sh, e.g.:
#   ~/predator-test/tools/start.sh cold cold.sh APP_DIR=$HOME/predator-test/app-v012p TAG=K
# Variables (VAR=value arguments):
#   APP_DIR  release folder that has run once (its game_code/ and shaders/ are copied)
#   TAG      prefix of the run names (default K)
#   CASES    "name|level|VAR=value ...;..." (level "-" = no level: menu start); default below
#   RUN_S    seconds of title time (FCP_EXIT_AFTER, default 60)
# Each run: runs/<TAG>_<name>/ like run.sh plus threads.txt (nice values of the game's threads in the first 12 s).
T=${PREDATOR_TEST:-$HOME/predator-test}
X=$T/tools/run.sh
for kv in "$@"; do export "$kv"; done
APP=${APP_DIR:-$T/app-v012p}
TAG=${TAG:-K}
N="FCP_RENDER_PIPELINE_SHIPPED=0"
CASES=${CASES:-"m_list|-|;m_none|-|$N;m_prio|-|FCP_RENDER_PREWARM_LOW_PRIORITY=0;d60_list|21_dunea|FCP_FPS=60;d60_none|21_dunea|FCP_FPS=60 $N;d30_list|21_dunea|FCP_FPS=30;d30_none|21_dunea|FCP_FPS=30 $N;c60_list|06_cliffs|FCP_FPS=60;c60_none|06_cliffs|FCP_FPS=60 $N;c30_list|06_cliffs|FCP_FPS=30;c30_none|06_cliffs|FCP_FPS=30 $N;t60_list|00_training|FCP_FPS=60;t60_none|00_training|FCP_FPS=60 $N;t30_list|00_training|FCP_FPS=30;t30_none|00_training|FCP_FPS=30 $N;d60_prio|21_dunea|FCP_FPS=60 FCP_RENDER_PREWARM_LOW_PRIORITY=0"}
log() { echo "$(date +%T) $*" >> "$T/runs/cold.txt"; }
gs() { pgrep -o gamescope-wl || pgrep -o -f '^gamescope '; }
C=$T/cold
log "cold start TAG=$TAG APP=$APP, gamescope $(gs)"
IFS=';' read -ra items <<< "$CASES"
for it in "${items[@]}"; do
  name=${it%%|*}; rest=${it#*|}; lv=${rest%%|*}; vars=${rest#*|}
  run=${TAG}_$name
  rm -rf "$C"; mkdir -p "$C"
  # the release files and the prepared game code / shader pack, nothing the game wrote while playing
  (cd "$APP" && for f in *; do case "$f" in game_code|shaders|compiler|licenses) ;; cache|logs|predator.log*|predator.gen*|pipelines.*.bin|vk_pipelines.bin|predator_input.toml) ;; *) cp -a "$f" "$C/";; esac; done)
  cp -a "$APP/game_code" "$APP/shaders" "$C/"
  ln -s "$APP/compiler" "$C/compiler"; ln -s "$APP/licenses" "$C/licenses"
  rm -rf "$T/mesa-cold"; mkdir -p "$T/mesa-cold"
  LV=""; [ "$lv" != "-" ] && LV="FCP_START_LEVEL=$lv"
  g=$(gs)
  log "$run: start ($lv $vars), user folder $(ls "$C" | tr '\n' ' ' | cut -c1-200)"
  ( # nice values of the game's threads while the prewarm runs (Wine maps thread priorities only with permission)
    for s in 3 6 9 12; do sleep 3; p=$(pgrep -f 'predator.exe' | head -1)
      [ -n "$p" ] && { echo "== t=$s pid $p"; ps -L -o tid=,nice=,pri=,pcpu=,comm= -p "$(pgrep -f 'predator.exe' | tr '\n' ',' | sed 's/,$//')"; }
    done > "$T/runs/$run.threads.txt" 2>&1 ) &
  # shellcheck disable=SC2086
  "$X" "$run" 130 APP_DIR=$C FCP_USER_ROOT_UNIX=$C MESA_SHADER_CACHE_DIR=$T/mesa-cold FCP_NO_DIALOGS=1 \
    FCP_FPS_STATS=5 FCP_CPU_STATS=5 FCP_TRACE_HITCHES=25 FCP_TRACE_FRAMES=1 FCP_INPUT_SCRIPT_CLOCK=level \
    FCP_INPUT_SCRIPT=10-50:lup FCP_EXIT_AFTER=${RUN_S:-60} $LV $vars
  wait
  mv "$T/runs/$run.threads.txt" "$T/runs/$run/threads.txt" 2>/dev/null
  [ "$(gs)" != "$g" ] && log "$run: GAMESCOPE CHANGED ($g -> $(gs))"
  log "$run: done ($(tail -1 "$T/runs/$run/run.txt" | cut -c10-))"
  sleep 5
done
rm -rf "$C" "$T/mesa-cold"
log "cold done, gamescope $(gs)"
