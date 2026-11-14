#!/bin/bash
# fps45_check.sh: frame rate corrections at 45 on the Steam Deck (docs/phase1/bildraten-45.md, section 8) with the
# Deck OLED's new default of 45, after a fresh first start (empty user folder, no FCP_FPS set):
#   first start   log "device: Steam Deck OLED default FCP_FPS = 45", the new predator.toml says fps = 45
#   land          m01_mercycreek, 7 jumps (FCP_TRACE_LAND=1), two runs
#   turn          m01_mercycreek, 1 s full right stick (FCP_TRACE_STATE=1), three runs
# Start: ~/predator-test/tools/start.sh f45 fps45_check.sh APP_DIR=$HOME/predator-test/app-v012m
T=${PREDATOR_TEST:-$HOME/predator-test}
X=$T/tools/run.sh
for kv in "$@"; do export "$kv"; done
A=${APP_DIR:-$T/app-v012m}
log() { echo "$(date +%T) $*" >> "$T/runs/f45.txt"; }
rm -rf "$A/game_code" "$A/shaders" "$A/cache" "$A/logs" "$A"/predator.log* "$A/predator.toml"
touch "$A/portable.txt"
printf '[game]\nimage = '"'"'Z:\\home\\deck\\predator-test\\assets\\predator.iso'"'"'\n' > "$A/predator.toml"
B="APP_DIR=$A FCP_USER_ROOT_UNIX=$A FCP_NO_DIALOGS=1 FCP_FPS_STATS=5"
"$X" F45_first 20 $B FCP_START_LEVEL=m01_mercycreek FCP_EXIT_AFTER=30 WAIT_LINE='framestat: .*level m01' MAX_WAIT=1800
log "first: $(grep -a 'first-start build in' "$T/runs/F45_first/predator.log" | sed 's/.*game code: //')"
log "first: $(grep -a 'shader pack .* built' "$T/runs/F45_first/predator.log" | sed 's/.*first start: //')"
log "first: $(grep -a 'default FCP_FPS = ' "$T/runs/F45_first/predator.log" | sed 's/^ *[0-9.]* *[0-9]* //')"
log "first: toml $(grep -E '^fps' "$A/predator.toml")"
cp "$A/predator.toml" "$T/runs/F45_first/predator.toml.after"
for n in 1 2; do
  "$X" F45_land_$n 90 $B FCP_START_LEVEL=m01_mercycreek FCP_TRACE_LAND=1 FCP_TRACE_STATE=1 \
    FCP_TEST_TELEPORT=22:202.839,199.301,26 FCP_INPUT_SCRIPT_CLOCK=level FCP_INPUT_SCRIPT=8:a,10:a,12:a,14:a,16:a,18:a,20:a \
    FCP_EXIT_AFTER=30
  log "land $n: $(grep -ac 'land: ' "$T/runs/F45_land_$n/predator.log") land lines"
done
for n in 1 2 3; do
  "$X" F45_turn_$n 90 $B FCP_START_LEVEL=m01_mercycreek FCP_TRACE_STATE=1 FCP_INPUT_SCRIPT_CLOCK=level \
    FCP_INPUT_SCRIPT=14-15:rright FCP_EXIT_AFTER=22
  log "turn $n done"
done
log "fps45 done"
