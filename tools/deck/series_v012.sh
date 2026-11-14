#!/bin/bash
# series_v012.sh: the Deck measurement for v0.1.2 (docs/phase1/deck-v0.1.2.md) in one systemd user unit:
#   1. pacing  (pacing.sh, TAG=V): per level a warm-up, then 30/45 (display pacing), 60 at 90 Hz, 60 at 60 Hz with the
#              Deck default 1280x800 native, and 45/60 with aspect 16:9 (1280x720 with bars) for the cost of 1280x800
#   2. spikes  (pacing.sh, TAG=S): two equal 120-s passes per level at 60 and at 45 with FCP_TRACE_FRAMES=1,
#              FCP_TRACE_HITCHES=20, REX_VSYNC_STATS=1 and the walk/turn/fire script of tools/spike_measure.ps1
#   3. cold    (cold.sh, TAG=K): shipped pipeline list against FCP_RENDER_PIPELINE_SHIPPED=0, cold
# Start: ~/predator-test/tools/start.sh v012 series_v012.sh APP_DIR=$HOME/predator-test/app-v012p [PARTS="1 2 3"]
T=${PREDATOR_TEST:-$HOME/predator-test}
for kv in "$@"; do export "$kv"; done
APP=${APP_DIR:-$T/app-v012p}
PARTS=${PARTS:-1 2 3}
D="FCP_FPS_PACING=display"
for p in $PARTS; do
  case $p in
  1)
    bash "$T/tools/pacing.sh" APP_DIR="$APP" TAG=V \
      CASES="d30|FCP_FPS=30 $D;d45|FCP_FPS=45 $D;f60|FCP_FPS=60;c60|FCP_FPS=60 GS_CYCLE=60;a45|FCP_FPS=45 $D FCP_RENDER_ASPECT=16:9;a60|FCP_FPS=60 FCP_RENDER_ASPECT=16:9"
    ;;
  2)
    W=""; for t in $(seq 10 20 119); do W="$W,$t-$((t + 14)):lup,$((t + 14))-$((t + 15)):rright:0.6,$((t + 16))-$((t + 18)):lup+rt"; done
    W=${W#,}
    S="FCP_TRACE_FRAMES=1 FCP_TRACE_HITCHES=20 REX_VSYNC_STATS=1 FCP_INPUT_SCRIPT=$W"
    bash "$T/tools/pacing.sh" APP_DIR="$APP" TAG=S WARM=0 RUN_S=120 \
      CASES="s60a|FCP_FPS=60 $S;s60b|FCP_FPS=60 $S;s45a|FCP_FPS=45 $D $S;s45b|FCP_FPS=45 $D $S"
    ;;
  3)
    bash "$T/tools/cold.sh" APP_DIR="$APP" TAG=K
    ;;
  esac
done
echo "series done $(date +%T)"
