#!/bin/bash
# extra_v012.sh: follow-up checks to the warm spike runs (docs/phase1/deck-v0.1.2.md), same walk/turn/fire script and
# traces as series_v012.sh part 2, 60 frames/s, warm:
#   r0   FCP_PAD_RUMBLE=0 (vibration strength 0) against the default, two passes each in 00_training (slow
#        XamInputSetState calls),
#   vp   REX_VSYNC_PRIORITY=2 against unset (vblank thread priority) in 21_dunea and 00_training
# Start: ~/predator-test/tools/start.sh extra extra_v012.sh APP_DIR=$HOME/predator-test/app-v012p
T=${PREDATOR_TEST:-$HOME/predator-test}
for kv in "$@"; do export "$kv"; done
APP=${APP_DIR:-$T/app-v012p}
W=""; for t in $(seq 10 20 119); do W="$W,$t-$((t + 14)):lup,$((t + 14))-$((t + 15)):rright:0.6,$((t + 16))-$((t + 18)):lup+rt"; done
W=${W#,}
S="FCP_FPS=60 FCP_TRACE_FRAMES=1 FCP_TRACE_HITCHES=20 REX_VSYNC_STATS=1 FCP_INPUT_SCRIPT=$W"
bash "$T/tools/pacing.sh" APP_DIR="$APP" TAG=R WARM=0 RUN_S=120 LEVELS="00_training" \
  CASES="r0a|$S FCP_PAD_RUMBLE=0;defa|$S;r0b|$S FCP_PAD_RUMBLE=0;defb|$S;vp2|$S REX_VSYNC_PRIORITY=2"
bash "$T/tools/pacing.sh" APP_DIR="$APP" TAG=R WARM=0 RUN_S=120 LEVELS="21_dunea" \
  CASES="vp2|$S REX_VSYNC_PRIORITY=2;def|$S"
echo "extra done $(date +%T)"
