#!/bin/bash
# issue1.sh: Issue #1 (graphics) on the Steam Deck's AMD GPU (docs/phase1/issue-1-grafik.md, sections 10 and 11):
# 03_river with the reporter's settings, enemies off, teleports to the spots of the video (medical crates, covered
# bridge from the shore, on the bridge, from the water, jetty), a frame dump every DUMP_EVERY frames and a gamescope
# screenshot every SHOT_EVERY seconds (what the screen shows).
# Start: ~/predator-test/tools/start.sh i1 issue1.sh APP_DIR=$HOME/predator-test/app-i1 NAME=i1_base [VAR=value ...]
#   APP_DIR     portable release folder (game_code/, shaders/ and portable.txt present)
#   NAME        run name (runs/<NAME>, shots in runs/<NAME>/gs)
#   VIEWS       file from issue1_views.py (fixed views, paired with the PC by issue1_pick.py); without it: one full
#               turn at each spot (first exploration run)
#   TOML        settings file copied to $APP_DIR/predator.toml before the run (default: the reporter's settings)
#   every other VAR=value is passed on to the game (e.g. FCP_RENDER_SCALE=1 to halve a setting)
T=${PREDATOR_TEST:-$HOME/predator-test}
X=$T/tools/run.sh
EXTRA=()
for kv in "$@"; do
  case "$kv" in APP_DIR=*|NAME=*|TOML=*|VIEWS=*|DUMP_EVERY=*|SHOT_EVERY=*|RUN_S=*) export "$kv";; *) EXTRA+=("$kv");; esac
done
A=${APP_DIR:-$T/app-i1}
NAME=${NAME:-i1}
DUMP_EVERY=${DUMP_EVERY:-45}
SHOT_EVERY=${SHOT_EVERY:-3}
RUN_S=${RUN_S:-150}
log() { echo "$(date +%T) $*" >> "$T/runs/issue1.txt"; }
if [ -n "$TOML" ]; then cp "$TOML" "$A/predator.toml"; else
cat > "$A/predator.toml" <<'EOF'
[game]
image = 'Z:\home\deck\predator-test\assets\predator.iso'

[display]
mode = "windowed"
width = 1366
height = 768

[graphics]
backend = "vulkan"
msaa = "console"
smaa = true
scale = 1.5
resolution = "scale"
EOF
fi
touch "$A/portable.txt"
if [ -n "$VIEWS" ]; then
  TP=$(sed -n 's/^FCP_TEST_TELEPORT=//p' "$VIEWS"); KS=$(sed -n 's/^FCP_KEY_SCRIPT=//p' "$VIEWS")
else
  # teleports (level seconds): crates 12, shore 26, bridge 40, water 54-68 (repeated every 0.5 s, the player sinks),
  # jetty 70, second crate spot 84, second shore spot 98; one turn (mx=3600) after each
  W=""; for i in $(seq 0 27); do W="$W|$(awk -v i=$i 'BEGIN{printf "%.1f", 54 + i * 0.5}'):338,580,18.4"; done
  TP="12:364.5,525,22.5|26:322,625,28.5|40:340,631.5,28.5${W}|70:353.8,553.2,22.5|84:361,521.5,22.5|98:360,618,28.5"
  KS="13-25:mx=3600,27-39:mx=3600,41-53:mx=3600,55-67:mx=3600,71-83:mx=3600,85-97:mx=3600,99-111:mx=3600"
fi
O=$T/runs/$NAME; rm -rf "$O"; mkdir -p "$O/gs"
( sleep 15; i=0; while [ ! -f "$O/gs/.done" ] && [ $i -lt 200 ]; do
    WAYLAND_DISPLAY=gamescope-0 XDG_RUNTIME_DIR=/run/user/1000 timeout 5 gamescopectl screenshot "$O/gs/gs_$(printf %03d $((15 + i * SHOT_EVERY))).png" > /dev/null 2>&1
    i=$((i + 1)); sleep "$SHOT_EVERY"; done ) &
shooter=$!
log "$NAME: start (${EXTRA[*]})"
"$X" "$NAME" "$RUN_S" APP_DIR="$A" FCP_USER_ROOT_UNIX="$A" FCP_NO_DIALOGS=1 FCP_FPS_STATS=5 \
  FCP_START_LEVEL=03_river FCP_INPUT_SCRIPT_CLOCK=level \
  'FCP_TEST_PATCH=EntityClass="Merc"|EntityClass="Merx"' \
  'FCP_TEST_PATCH2=EntityClass="CreatureGenerator"|EntityClass="CreatureGeneratoX"' \
  "FCP_TEST_TELEPORT=$TP" "FCP_KEY_SCRIPT=$KS" \
  "FCP_DUMP_FRAMES=f_%d.png" "FCP_DUMP_FRAME_EVERY=$DUMP_EVERY" FCP_RENDER_STATS=450 \
  FCP_EXIT_AFTER=$RUN_S "${EXTRA[@]}"
touch "$O/gs/.done"; kill $shooter 2>/dev/null
cp "$A/predator.toml" "$O/predator.toml.used"
log "$NAME: end, $(ls "$O/logs" 2>/dev/null | grep -c png) dumps, $(ls "$O/gs" | wc -l) screenshots"
