#!/bin/bash
# rumble_ab.sh: SDL3 pads without SDL calls on the guest threads (rexglue UPSTREAM 301) against REX_HID_SDL_SYNC=1
# (the former synchronous calls), same build, warm, walk/turn/fire script with vibration, traces as series_v012.sh
# part 2 (docs/phase1/deck-erststart-rumble.md). Alternating per level: sync/async at 60 and 45, two passes.
# Start: ~/predator-test/tools/start.sh rab rumble_ab.sh APP_DIR=$HOME/predator-test/app-v012r
T=${PREDATOR_TEST:-$HOME/predator-test}
for kv in "$@"; do export "$kv"; done
APP=${APP_DIR:-$T/app-v012r}
W=""; for t in $(seq 10 20 119); do W="$W,$t-$((t + 14)):lup,$((t + 14))-$((t + 15)):rright:0.6,$((t + 16))-$((t + 18)):lup+rt"; done
W=${W#,}
S="FCP_TRACE_FRAMES=1 FCP_TRACE_HITCHES=20 FCP_INPUT_SCRIPT=$W"
D="FCP_FPS_PACING=display"
C=""
for p in a b; do
  C="$C;s60$p|FCP_FPS=60 REX_HID_SDL_SYNC=1 $S;n60$p|FCP_FPS=60 $S;s45$p|FCP_FPS=45 $D REX_HID_SDL_SYNC=1 $S;n45$p|FCP_FPS=45 $D $S"
done
bash "$T/tools/pacing.sh" APP_DIR="$APP" TAG=H WARM=1 RUN_S=120 LEVELS="${LEVELS:-00_training 21_dunea}" CASES="${C#;}"
echo "rumble_ab done $(date +%T)"
