#!/bin/bash
# shots.sh: screenshots of what gamescope shows (the composed 1280x800 output, `gamescopectl screenshot`) while the
# game runs, plus the game window's geometry, for the picture check on the Deck (docs/phase1/deck-v0.1.2.md).
# Start through start.sh: ~/predator-test/tools/start.sh shots shots.sh APP_DIR=$HOME/predator-test/app-v012p
# Pictures land in ~/predator-test/shots/<run>_<s>.png (never in the repository).
T=${PREDATOR_TEST:-$HOME/predator-test}
for kv in "$@"; do export "$kv"; done
APP=${APP_DIR:-$T/app-v012p}
mkdir -p "$T/shots"
export XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:0
shoot() {  # shoot <run> <seconds...>: screenshots at these seconds after the start of the run
  local run=$1 last=0; shift
  for s in "$@"; do
    sleep $((s - last)); last=$s
    WAYLAND_DISPLAY=gamescope-0 timeout 5 gamescopectl screenshot "$T/shots/${run}_$s.png" > /dev/null 2>&1
    w=$(xwininfo -root -children 2>/dev/null | grep -i '"predator' | awk '{print $1}' | head -1)
    [ -n "$w" ] && echo "$run $s: $(xwininfo -id "$w" | grep -E 'Width|Height|Absolute' | tr -s ' ' | tr '\n' ' ')" >> "$T/shots/geometry.txt"
  done
}
B="APP_DIR=$APP FCP_USER_ROOT_UNIX=$APP FCP_NO_DIALOGS=1 FCP_FPS=45 FCP_FPS_STATS=5"
# 1. menus: title, main menu, F10 menu (opened and closed with the key script)
shoot menu 8 16 24 32 40 &
"$T/tools/run.sh" shots_menu 60 $B FCP_SKIP_INTRO=1 FCP_EXIT_AFTER=50 \
  FCP_INPUT_SCRIPT="12:start,20:a" FCP_KEY_SCRIPT="28:f10,36:f10"
wait
# 2. level 21_dunea: HUD while walking, pause menu, level picture with aspect 16:9 for comparison
shoot dunea 20 28 36 &
"$T/tools/run.sh" shots_dunea 60 $B FCP_START_LEVEL=21_dunea FCP_INPUT_SCRIPT_CLOCK=level \
  FCP_INPUT_SCRIPT="10-24:lup,24-25:rright:0.6,26-27:lup+rt,30:start" FCP_EXIT_AFTER=45
wait
shoot dunea169 20 28 &
"$T/tools/run.sh" shots_dunea169 50 $B FCP_START_LEVEL=21_dunea FCP_RENDER_ASPECT=16:9 FCP_INPUT_SCRIPT_CLOCK=level \
  FCP_INPUT_SCRIPT="10-24:lup" FCP_EXIT_AFTER=35
wait
ls -la "$T/shots"
