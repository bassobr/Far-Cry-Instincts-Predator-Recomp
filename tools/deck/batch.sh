#!/bin/bash
# batch.sh: the Steam Deck test series (first and second start, frame rates, both backends, picture dumps), step by step (STEPS=a,b,...; default all).
# Start through start.sh (systemd user unit):  ~/predator-test/tools/start.sh b1 batch.sh STEPS=first
# Status in ~/predator-test/runs/batch.txt. Each run: run.sh (one Proton run in gamescope), results in runs/<name>/.
# Guards as FCI/FCE's batch: no start without gamescope; a changed gamescope PID after a run counts as a crash; the
# second crash stops the batch.
T=${PREDATOR_TEST:-$HOME/predator-test}
X=$T/tools/run.sh
for kv in "$@"; do export "$kv"; done
STEPS=${STEPS:-first,second,dxil,perf90,perf6030,dumps}
ROUNDS=${ROUNDS:-2}
LEVELS=${LEVELS:-21_dunea 06_cliffs 00_training}
log() { echo "$(date +%T) $*" >> "$T/runs/batch.txt"; }
has() { case ",$STEPS," in *",$1,"*) return 0;; esac; return 1; }
gs() { pgrep -o gamescope-wl || pgrep -o -f '^gamescope '; }
crashes=0; last=$(gs)
# common to the measured runs: dev start, statistics every 5 s, walk forward from level second 10 to 50
M="FCP_NO_DIALOGS=1 FCP_FPS_STATS=5 FCP_CPU_STATS=5 FCP_INPUT_SCRIPT_CLOCK=level FCP_INPUT_SCRIPT=10-50:lup"
one() {  # one <name> <seconds> <vars...>
  local name=$1 g g2 t0
  g=$(gs)
  [ -z "$g" ] && { log "$name: no gamescope, not started"; return 1; }
  t0=$(date '+%Y-%m-%d %H:%M:%S')
  log "$name: start, gamescope $g"
  "$X" "$@"
  g2=$(gs)
  journalctl --since "$t0" --no-pager 2>/dev/null | grep -aiE "gamescope|coredump|SIGABRT|Assertion|dumped core" | tail -40 > "$T/runs/$1/journal.txt"
  log "$name: done ($(tail -1 "$T/runs/$1/run.txt" | cut -c10-)), journal lines $(wc -l < "$T/runs/$1/journal.txt")"
  if [ "$g2" != "$g" ]; then
    crashes=$((crashes + 1)); log "$name: GAMESCOPE CHANGED ($g -> '$g2'), crash $crashes"
    [ $crashes -ge 2 ] && { log "second crash: stop"; exit 2; }
    sleep 30
  fi
  sleep 5
}
log "batch start STEPS=$STEPS ROUNDS=$ROUNDS, gamescope $last, proton '${PROTON_NAME:-Proton 10.0}'"
# first start: game code build + shader pack, then 21_dunea for 90 s (title run time)
if has first; then
  one first 150 "WAIT_LINE=first start: shader pack .* built in|shader pack FAILED|build FAILED" MAX_WAIT=2400 \
    $M FCP_START_LEVEL=21_dunea FCP_FPS=60 FCP_EXIT_AFTER=90
fi
# second start: the DLL is found, no build
if has second; then one second 60 $M FCP_START_LEVEL=21_dunea FCP_FPS=60 FCP_EXIT_AFTER=40; fi
# first D3D12 start: builds the DXIL shader pack (about as long as the SPIR-V one, longer than a measured run's
# limit), so it gets its own run that waits for the pack before the measured runs
if has dxil; then
  one dxil 40 "WAIT_LINE=first start: shader pack .* built in|shader pack FAILED" \
    MAX_WAIT=900 $M FCP_START_LEVEL=21_dunea FCP_FPS=60 FCP_RENDER_BACKEND=d3d12 FCP_EXIT_AFTER=40
fi
# 90 fps, both backends, all levels, ROUNDS rounds (round 1 also warms the pipeline caches)
if has perf90; then
  for r in $(seq 1 $ROUNDS); do for lv in $LEVELS; do for be in vulkan d3d12; do
    one "p90_${lv}_${be}_$r" 120 $M FCP_START_LEVEL=$lv FCP_FPS=90 FCP_RENDER_BACKEND=$be FCP_EXIT_AFTER=${RUN_S:-80}
  done; done; done
fi
# 60 and 30 fps (and unlimited) with BACKEND (default vulkan), all levels; 21_dunea also with the other backend
if has perf6030; then
  be=${BACKEND:-vulkan}; other=d3d12; [ "$be" = d3d12 ] && other=vulkan
  for fps in 60 30 unlimited; do for lv in $LEVELS; do
    one "p${fps}_${lv}_${be}" 120 $M FCP_START_LEVEL=$lv FCP_FPS=$fps FCP_RENDER_BACKEND=$be FCP_EXIT_AFTER=${RUN_S:-80}
  done; done
  one "p60_21_dunea_${other}" 120 $M FCP_START_LEVEL=21_dunea FCP_FPS=60 FCP_RENDER_BACKEND=$other FCP_EXIT_AFTER=${RUN_S:-80}
fi
# picture dumps, both backends, every DUMP_EVERY-th frame
if has dumps; then
  for lv in $LEVELS; do for be in vulkan d3d12; do
    one "d_${lv}_${be}" 90 $M FCP_START_LEVEL=$lv FCP_FPS=60 FCP_RENDER_BACKEND=$be FCP_EXIT_AFTER=60 \
      FCP_DUMP_FRAMES=dump_%d.png FCP_DUMP_FRAME_EVERY=${DUMP_EVERY:-900}
  done; done
fi
# extra runs: EXTRA="name|vars;name|vars" (vars separated by spaces)
if has extra; then
  IFS=';' read -ra items <<< "$EXTRA"
  for it in "${items[@]}"; do one "${it%%|*}" 120 $M ${it#*|}; done
fi
log "batch done, crashes $crashes, gamescope $(gs)"
