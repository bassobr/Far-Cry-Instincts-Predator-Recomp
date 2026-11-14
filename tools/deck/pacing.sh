#!/bin/bash
# pacing.sh: frame pacing series on the Steam Deck (docs/phase1/deck-ruckeln.md): frame rates and pacing modes per
# level, each run with the game's frame times (framestat) and the display's view (presentstat, present wait).
# Start through start.sh (systemd user unit), e.g.:
#   ~/predator-test/tools/start.sh pc pacing.sh APP_DIR=$HOME/predator-test/app-dev TAG=a
# Variables (VAR=value arguments):
#   APP_DIR  portable release folder (default ~/predator-test/app-dev; its game_code/ and shaders/ must exist)
#   TAG      prefix of the run names (default p); runs land in ~/predator-test/runs/<TAG>_<case>_<level>
#   LEVELS   levels (default "21_dunea 00_training 06_cliffs 01_beach")
#   CASES    "name|VAR=value VAR=value;name|..." (default below: the player's 60, then 30/45/90 with and without
#            display pacing); a case with GS_REFRESH=<hz> asks gamescope for that refresh rate (run.sh)
#   WARM     1 (default): one unmeasured warm-up run per level first (pipeline caches as on a second visit)
#   RUN_S    seconds in the level (FCP_EXIT_AFTER, default 75); measured from level second 10 on (pacing_summary.py)
# Results: pacing_summary.py on the pulled runs folder. Status in ~/predator-test/runs/pacing.txt.
T=${PREDATOR_TEST:-$HOME/predator-test}
X=$T/tools/run.sh
for kv in "$@"; do export "$kv"; done
APP=${APP_DIR:-$T/app-dev}
TAG=${TAG:-p}
LEVELS=${LEVELS:-21_dunea 00_training 06_cliffs 01_beach}
WARM=${WARM:-1}
# Default set (docs/phase1/deck-ruckeln.md): at the Deck's 90 Hz the player's 60, the console's 30, 45 and 90, each
# with the game's own pacing (f) and display pacing (d); at 60 Hz (GS_CYCLE=60) 60 and 30. FCP_FPS_PACING=game
# forces the game's own pacing also where a device default chooses display pacing.
G="FCP_FPS_PACING=game"; D="FCP_FPS_PACING=display"
CASES=${CASES:-"f60|FCP_FPS=60 $G;f30|FCP_FPS=30 $G;d30|FCP_FPS=30 $D;f45|FCP_FPS=45 $G;d45|FCP_FPS=45 $D;f90|FCP_FPS=90 $G;d90|FCP_FPS=90 $D;c60|FCP_FPS=60 $G GS_CYCLE=60;cd60|FCP_FPS=60 $D GS_CYCLE=60;cd30|FCP_FPS=30 $D GS_CYCLE=60"}
log() { echo "$(date +%T) $*" >> "$T/runs/pacing.txt"; }
gs() { pgrep -o gamescope-wl || pgrep -o -f '^gamescope '; }
crashes=0
# common: portable folder, dev start, statistics every 5 s, walk forward from level second 10 to 50
M="APP_DIR=$APP FCP_USER_ROOT_UNIX=$APP FCP_NO_DIALOGS=1 FCP_FPS_STATS=5 FCP_CPU_STATS=5 FCP_INPUT_SCRIPT_CLOCK=level FCP_INPUT_SCRIPT=10-50:lup FCP_EXIT_AFTER=${RUN_S:-75}"
one() {  # one <name> <vars...>
  local name=$1 g g2 t0
  shift
  g=$(gs)
  [ -z "$g" ] && { log "$name: no gamescope, not started"; return 1; }
  t0=$(date '+%Y-%m-%d %H:%M:%S')
  log "$name: start ($*)"
  "$X" "$name" 130 $M "$@"
  g2=$(gs)
  journalctl --since "$t0" --no-pager 2>/dev/null | grep -aiE "gamescope|coredump|SIGABRT|Assertion|dumped core" | tail -40 > "$T/runs/$name/journal.txt"
  log "$name: done ($(tail -1 "$T/runs/$name/run.txt" | cut -c10-))"
  if [ "$g2" != "$g" ]; then
    crashes=$((crashes + 1)); log "$name: GAMESCOPE CHANGED ($g -> '$g2'), crash $crashes"
    [ $crashes -ge 2 ] && { log "second crash: stop"; exit 2; }
    sleep 30
  fi
  sleep 5
}
log "pacing start TAG=$TAG APP=$APP LEVELS='$LEVELS' WARM=$WARM, gamescope $(gs)"
IFS=';' read -ra items <<< "$CASES"
for lv in $LEVELS; do
  [ "$WARM" = 1 ] && one "${TAG}_warm_${lv}" FCP_START_LEVEL=$lv FCP_FPS=60
  for it in "${items[@]}"; do
    # shellcheck disable=SC2086
    one "${TAG}_${it%%|*}_${lv}" FCP_START_LEVEL=$lv ${it#*|}
  done
done
log "pacing done, crashes $crashes, gamescope $(gs)"
